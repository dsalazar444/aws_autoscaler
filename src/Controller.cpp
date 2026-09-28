#include "Controller.h"
#include "AWSMetricsSource.h"
#include "FakeMetricsSource.h"
#include "LinearRegressionPredictor.h"
#include "Analytics.h"
#include "Validator.h"

#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>

using namespace std;
using namespace Analytics; // De Analytics.h
using namespace ValidatorUtils; // De Validator.h
using namespace scaleAction; // De DecidedAction.h

using json = nlohmann::json;

namespace {
    std::optional<double> GetCurrentGlobalCpu(const MetricSeries& history,
        std::chrono::system_clock::time_point now,
        std::chrono::seconds tolerance = std::chrono::seconds(15) ){ // TODO: Pedir ese timestamp por json, y pasarlo en las funciones, creo que son las de calcularGlobal, y las que usen expected timestamps
    
        const MetricSample& current = history.back();
        auto difference = now > current.timestamp
            ? now - current.timestamp
            : current.timestamp - now;

        if (difference <= tolerance) {
            return current.value;
        }

        return std::nullopt;
    }
}
Controller::Controller(string configFile)
    : _config(LoadConfig(configFile)),
      _state(State::Idle),
      _falseData(_config["falseData"].get<bool>()),
      _asg(_config["aws"]["asgName"].get<string>()),
      _targetGroupArn(_config["aws"]["tgArn"].get<string>()),
      _instanceIds(),
      _metricsSource(nullptr),
      _idsSource(make_unique<AWSMetricsSource>(_asg, _targetGroupArn)),
      _fakeMetricsSource(nullptr),

      _minInstances(_config["minInstances"].get<int>()),
      _maxInstances(_config["maxInstances"].get<int>()),

      _highThresholdCpu(_config["highThresholdCpu"].get<double>()),
      _lowThresholdCpu(_config["lowThresholdCpu"].get<double>()),
      _highThresholdReq(_config["highThresholdReq"].get<int>()),
      _lowThresholdReq(_config["lowThresholdReq"].get<int>()),

      _historyWindow(chrono::seconds{stoll(_config["historyWindow"].get<string>())}),
      _horizonWindow(chrono::seconds{stoll(_config["horizonWindow"].get<string>())}),
      
      _sustainedHighWindowCpu(chrono::seconds{stoll(_config["sustainedHighWindowCpu"].get<string>())}),
      _sustainedLowWindowCpu(chrono::seconds{stoll(_config["sustainedLowWindowCpu"].get<string>())}),
      _sustainedHighWindowReq(chrono::seconds{stoll(_config["sustainedHighWindowReq"].get<string>())}),

      _queryPeriod(chrono::seconds{stoll(_config["queryPeriod"].get<std::string>())}),

      _scaleOutCooldown(chrono::seconds{stoll(_config["scaleOutCooldown"].get<std::string>())}),
      _scaleInCooldown(chrono::seconds{stoll(_config["scaleInCooldown"].get<std::string>())}),
      _cooldownUntil(), // Se le agregará valor a medida que accion se ejecute exitosamente

      _rng(std::random_device{}()),

      _valuePredictor(make_shared<LinearRegressionPredictor>()),
      _validator(_valuePredictor),
      _proactive(_valuePredictor, _horizonWindow, _highThresholdCpu),
      _reactive(_highThresholdCpu, _lowThresholdCpu, _highThresholdReq, _lowThresholdReq, _sustainedHighWindowCpu, _sustainedLowWindowCpu, _sustainedHighWindowReq),
      _actioner(_asg),
      _logger(_config["logPathFile"].get<std::string>()) { // el logger es capaz de convertir de string a path, que es lo que espera su constructor

    // Inicializar nullptr (_metricsSource y _fakeMetricsSource) según _falseData
    if (_falseData) {
        // puntero apunta a una o otra
        // instanceids se pasa vacio -> no problema porque no usaremos instanceIds de este, sino de Controller
        _metricsSource = make_unique<FakeMetricsSource>(_instanceIds);
        
        // Neceistamos acceder a métodos propios de Fake, pero _metricSource es IMetric, y no tiene 
        // setCpuOverride -> usamos un dynamic_pointer -> apuntará a mismo objeto que _metricSource
        // pero será de tipo fake -> nos permite acceder a métodos especificos -> NOTA: No es la mejor práctica
        // deberia separarse ese método a otra interfaz
        
        // _metricsSource es el dueño del objeto.
        // _fakeMetricsSource solo permite acceder a métodos específicos de FakeMetricsSource.
        // No es dueño y no debe sobrevivir a _metricsSource.
        _fakeMetricsSource =  dynamic_cast<FakeMetricsSource*>(_metricsSource.get());

    } else {
        _metricsSource = make_unique<AWSMetricsSource>(_asg, _targetGroupArn);
    }
    // NOTA: IMetricSource no deberia tener getInstanceIds, son responsabilidades diferentes -> IMetric solo deberia 
    // generar métricas, nada más. Deberian ser dos modulos -> IMetrics e IInstanceSource, que puede implementar solo la de AWS
    // porque sí o sí, los ids se toman de AWS, no se simulan -> No usaremos GetInstanceId de FakeMetrics
}

json Controller::LoadConfig(const std::string& configFile) {
    
    ifstream f(configFile); // crea objeto que abre archivo

    if (!f.is_open()) {
        throw runtime_error("Could not open config file: " + configFile);
    }

    // con parse leemos archivo, y trae su contenido json
    json configData = json::parse(f);
}


bool Controller::GetInstanceIds(){
    // Independientemente de si es falseData o no, se obtendrán los datos de un asg group real
    // se buscan datos falsos, pero acciones verdaderas

    FetchResult<vector<string>> ResultInstancesIds = _idsSource->GetInstanceIds();

    if (!ResultInstancesIds.IsOk()){
        return false;
    }

    // Puede ser una lista vacia, pero esa lista vacia se debe a porque lit no hay instancias
    // que es muy diferente a si fuera lista vacia por error de API, que descartamos arriba
    _instanceIds = move(ResultInstancesIds.value);

    return true;
}

// GetCurrents tienen sus propias funciones para poder implementar setOVerrideMetric
FetchResult<CurrentCpuSnapshot> Controller::GetCurrentCpus(){

    uniform_int_distribution<int> chance(1, 5); // genera int -> 1 tiene probabilidad de salir de un 20%

    if (_fakeMetricsSource && chance(_rng) == 1){
        // value aleatorio
        uniform_real_distribution<double> valueDist(10.0, 60.0);
        double value = valueDist(_rng);

        // id aleatorio
        uniform_int_distribution<size_t> indexDist(0, _instanceIds.size() - 1);
        size_t index = indexDist(_rng);
        string id = _instanceIds[index];

        // Neceistamos acceder a método propio de Fake, pero _metricSource es IMetric, y no tiene 
        // setCpuOverride -> usamos un dinamic_pointer -> apuntará a mismo objeto que _metricSource
        // pero será de tipo fake -> nos permite acceder a métodos especificos -> NOTA: No es la mejor práctica
        // deberia separarse ese método a otra interfaz
        _fakeMetricsSource->SetCpuOverride(id, value);

    }

    // sea fake o aws, se obtienen metricas
    return _metricsSource->GetCurrentCpus(_instanceIds);
}

FetchResult<double> Controller::GetCurrentRequest(){

    uniform_int_distribution<int> chance(1, 5); // genera int -> 1 tiene probabilidad de salir de un 20%

    if (_fakeMetricsSource && chance(_rng) == 1){
        // value aleatorio
        uniform_real_distribution<double> valueDist(30.0, 500.0); // valores random
        double value = valueDist(_rng);

        _fakeMetricsSource->SetRequestOverride(value);

    }

    // sea fake o aws, se obtienen metricas
    return _metricsSource->GetCurrentRequest();
}

// Necesitamos esta funcion porque se puede salir de lifecycle sin llegar a crear
// objeto DecidedAction, entonces se debe manejar el log
void Controller::BuildAndSendRecord(std::chrono::system_clock::time_point now, optional<std::chrono::seconds> window,
                int instanceCount, std::optional<double> currentGlobalCpu,
                std::optional<double> currentGlobalReq, const std::string& justification,
                std::optional<ReactiveSignal> reactiveSignalCpu, std::optional<ReactiveSignal> reactiveSignalReq,
                std::optional<ProactiveSignal> proactiveSignal,
                std::optional<double> proactiveEstimatedCpu, int targetCount,
                std::optional<std::string> idToDelete, scaleAction::Action decision, 
                std::optional<ActionResult> actionResult) {

    Logger::DecisionRecord record;

    record.timestamp = now;
    // record.epochSeconds se calcula en logger
    // record.metricsConsidered en esta version siempre tendrán valor por defecto
    record.analyzedWindow = window; // Normalmente será _historyWindow
    
    record.instanceCountBefore = instanceCount;
    record.currentGlobalCpu = currentGlobalCpu;
    record.currentGlobalReq = currentGlobalReq;

    record.reactiveSignalCpu = reactiveSignalCpu;
    record.reactiveSignalReq = reactiveSignalReq;
    record.proactiveSignal = proactiveSignal;
    record.proactiveEstimatedCpu = proactiveEstimatedCpu;
    record.targetCount = targetCount;
    record.idToDelete = idToDelete;

    record.decision = decision;
    record.justification = justification;
    record.actionResult = actionResult;  
    
    _logger.Log(record);

    if (!actionResult.has_value()){
        return;
    }

    if (actionResult.value() == ActionResult::Success) {
        _state = State::CooldownOut;
        _cooldownUntil = now + _scaleOutCooldown;
    }
}

void Controller::LifeCycle(std::chrono::system_clock::time_point now){

    
    // Paso 1: Obtener instancias actuales -> En cada ciclo, se debe hacer
    if (!GetInstanceIds()){
        // Usamos -1 para indicar que no es que hayan 0 instancias, es que no las pudimos consultar
        // No causará nuingun problema, pues ese valor es solo para log, no para calculos
        BuildAndSendRecord(now, _historyWindow, -1, std::nullopt, std::nullopt, 
            "Fallo la consulta de instancias a AWS tras agotar reintentos", std::nullopt, 
            std::nullopt, std::nullopt, std::nullopt, -1, std::nullopt, Action::Mantain, std::nullopt);
        
        return;
    }

    int totalActualInstances = static_cast<int>(_instanceIds.size());

    // --- Paso 2: snapshot actual de CPU y Request
    FetchResult<CurrentCpuSnapshot> currentCpus = GetCurrentCpus();

    if (!currentCpus.IsOk()) {
        BuildAndSendRecord(now, _historyWindow, totalActualInstances, std::nullopt, std::nullopt, 
            "Fallo la consulta de CPU actual a AWS  tras agotar reintentos", std::nullopt, std::nullopt, 
            std::nullopt, std::nullopt, totalActualInstances, std::nullopt, Action::Mantain, std::nullopt);
        
        //LogAndMaintain(now, currentCount, "fallo la consulta de CPU actual a AWS tras agotar reintentos");
        return;
    }

    FetchResult<double> currentRequest = GetCurrentRequest();

    if (!currentRequest.IsOk()) {

        BuildAndSendRecord(now, _historyWindow, totalActualInstances, std::nullopt, std::nullopt,
            "Fallo la consulta de Request actual a AWS tras agotar reintentos", std::nullopt, std::nullopt, std::nullopt, 
            std::nullopt, totalActualInstances, std::nullopt, Action::Mantain, std::nullopt); 

        return;
    }

    // --- Paso 4: historico de CPU y de Request, con ventana suficiente para Reactivo y
    // Proactivo ---

    FetchResult<MetricSeriesByInstance> historyCpus = _metricsSource->GetCpuHistory(_instanceIds, _historyWindow);
    
    if (!historyCpus.IsOk()) {
        
        BuildAndSendRecord(now, _historyWindow, totalActualInstances, std::nullopt, currentRequest.value, 
            "Fallo la consulta de historico de CPU a AWS tras agotar reintentos", std::nullopt, std::nullopt, std::nullopt, 
            std::nullopt, totalActualInstances, std::nullopt, Action::Mantain, std::nullopt); 

        return;
    }
    
    FetchResult<MetricSeries> historyRequest = _metricsSource->GetRequestHistory(_historyWindow);
    
    if (!historyRequest.IsOk()) {
        BuildAndSendRecord(now, _historyWindow, totalActualInstances, std::nullopt, currentRequest.value, 
            "Fallo la consulta de historico de Request a AWS tras agotar reintentos", std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, totalActualInstances, std::nullopt, Action::Mantain, std::nullopt); 

        return;
    }

    // --- Paso 5: validar el historico completo (rellenar huecos, o
    // descartar el ciclo si falta demasiado) ---

    ValidationResult<MetricSeriesByInstance> validatedCpuHistory = _validator.ValidateCpuHistory(_instanceIds, historyCpus.value, now, _historyWindow, _queryPeriod);
    if (validatedCpuHistory.status == ValidationStatus::InsufficientData) {
        // esto va a pasar seguido en los primeros ciclos del sistema, mientras
        // se acumula suficiente historico  (no llegamos ni siquiera a Analyzers)--  la respuesta es  mantener y registrar por que

        BuildAndSendRecord(now, _historyWindow, totalActualInstances, std::nullopt, currentRequest.value, 
            "Historico de CPU insuficiente para evaluar (posiblemente el sistema esta arrancando)", std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, totalActualInstances, std::nullopt, Action::Mantain, std::nullopt); 

        return;
    }

    ValidationResult<MetricSeries> validatedRequestHistory = _validator.ValidateRequestHistory(historyRequest.value, now, _historyWindow, _queryPeriod);
    if (validatedRequestHistory.status == ValidationStatus::InsufficientData) {
        
        BuildAndSendRecord(now, _historyWindow, totalActualInstances, std::nullopt, currentRequest.value, 
            "Historico de Request insuficiente para evaluar (posiblemente el sistema esta arrancando)", std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, totalActualInstances, std::nullopt, Action::Mantain, std::nullopt); 

        return;
    }

    // el snapshot actual tambien se valida, pero solo hace falta si mas
    // adelante se decide reducir (para elegir que instancia remover) -- si
    // viene insuficiente, NO se bloquea todo el ciclo aqui; se maneja mas
    // adelante, solo si de verdad se llega a necesitar

    // NOTA: Usamos validatedCpusHistory, y no cpuHistory, porque aprovechamos que ya esta validada
    // y nunca será vacia, porque si llega hasta acá, es porque paso if de insufficientData
    ValidationResult<unordered_map<string, double>> validatedCurrentCpu = _validator.ValidateCurrentCpus(
        _instanceIds, currentCpus.value.valuesByInstance, validatedCpuHistory.value,
        currentCpus.value.timestamp);
    
    // --- Paso 6: reducir el historico de CPU por instancia a UNA serie global (p95) ---
    // solo CPU, porque Request ya vienen promediados

    // reutilizamos el MISMO grid que uso Validator para rellenar, asi los
    // buckets calzan entre lo que Validator entrego y lo que Analytics agrupa
    // -> El expectedTimestamp  que espera CalculatGlobalSeries debe coincidir con los que se "creó" el HistoryCpus
    vector<chrono::system_clock::time_point> expectedTimestamps = ValidatorUtils::BuildExpectedTimestamps(now, _historyWindow, _queryPeriod);
    MetricSeries globalCpusHistory = CalculateGlobalCpuSeries(validatedCpuHistory.value, expectedTimestamps);
    
    if (globalCpusHistory.empty()) {

        BuildAndSendRecord(now, _historyWindow, totalActualInstances, std::nullopt, currentRequest.value, 
            "Ningun bucket del historico de CPU validado tuvo dato de ninguna instancia -- no hay serie global de CPU que evaluar", std::nullopt, std::nullopt,
            std::nullopt, std::nullopt, totalActualInstances, std::nullopt, Action::Mantain, std::nullopt); 

        return;
    }

    // Obtenemos currentCpuGlobal
    optional<double> currentCpuGlobal = GetCurrentGlobalCpu(globalCpusHistory, now);

    // Siempre obtendremos metricas, pero si estamos en coolingdown, no tomamos acciones
    if (_state != State::Idle) {
        if (now < _cooldownUntil) {
    
            BuildAndSendRecord(now, _historyWindow, totalActualInstances, currentCpuGlobal, currentRequest.value, 
                "En cooldown, esperando a que se estabilice la ultima accion", std::nullopt, std::nullopt, std::nullopt, 
                std::nullopt, totalActualInstances, std::nullopt, Action::Mantain, std::nullopt);

            return;
        }
        // el cooldown ya expiro -- volvemos a Idle y seguimos evaluando
        // este mismo ciclo con datos frescos
        _state = State::Idle;
    }
    // Ya que estamos en Idle, evaluamos y ejecutamos
    EvaluateAndDecide(globalCpusHistory, validatedCurrentCpu, historyRequest.value, now, currentCpuGlobal, currentRequest, totalActualInstances);
}


void Controller::EvaluateAndDecide(const MetricSeries& globalCpusHistory,
                ValidationResult<std::unordered_map<std::string, double>> validatedCurrentCpu,
                const MetricSeries& historyRequest,
                std::chrono::system_clock::time_point now,
                std::optional<double> currentGlobalCpu,
                FetchResult<double> currentRequest,
                int totalActualInstances){
    
    // --- Paso 7: preguntarle a Reactivo y Proactivo ---

    // Proactive -> solo CPU
    ProactiveEvaluation proactiveOpinion = _proactive.Evaluate(globalCpusHistory, now);

    // Reactive -> CPU y Request
    ReactiveSignal reactiveOpinionCpu = _reactive.Evaluate("cpu", globalCpusHistory, now);
    ReactiveSignal reactiveOpinionReq = _reactive.Evaluate("req", historyRequest, now);
    
    DecidedAction action = Decide(proactiveOpinion, reactiveOpinionCpu, reactiveOpinionReq, currentGlobalCpu, validatedCurrentCpu, totalActualInstances);
    
    optional<ActionResult> actionResult = Act(action);

    // Acá se genera el log de acción
    BuildAndSendRecord(now, std::nullopt, totalActualInstances, currentGlobalCpu, currentRequest.value, 
        action.justification, reactiveOpinionCpu, reactiveOpinionReq, proactiveOpinion.signal, proactiveOpinion.estimatedValue, 
        action.targetCount, action.idToDelete, action.action, actionResult); 

}

scaleAction::DecidedAction Controller::Decide(const ProactiveEvaluation proactiveOpinion, 
            const ReactiveSignal reactiveOpinionCpu,
            const ReactiveSignal reactiveOpinionReq, 
            std::optional<double> currentGlobalCpu,
            ValidationResult<std::unordered_map<std::string, double>> validatedCurrentCpu,
            int totalActualInstances){
    


    // Reglas
    if (reactiveOpinionCpu == ReactiveSignal::InsufficientData) {
        // Si reactive no tiene suficiente data, menos lo tendrá proactive, pues su ventana
        // es más grande (horizonte). Y tampoco lo que diga reactiveRequest, pues está solo es decisiva
        // si reactiveCpu es Normal
    
        return {scaleAction::Action::Mantain, totalActualInstances, nullopt, "Reactivo no tiene suficiente ventana cubierta todavia para decidir estado sostenido"};
    }
    
    // --- Paso 8: SCALE-OUT -- OR entre Reactivo y Proactivo ---

    // Nota: No verificamos min ni max de instancias, porque queremos loggear eso.
    // CalculateMinimumSafeInstanceCount da siempre entre rangos, y cuando se calcula sin este, tambien se verifica
    bool triggersScaleOut = (reactiveOpinionCpu == ReactiveSignal::SustainedHigh 
                || proactiveOpinion.signal == ProactiveSignal::PredictsAboveHighThreshold)
            || (reactiveOpinionCpu == ReactiveSignal::Normal 
                && reactiveOpinionReq == ReactiveSignal::SustainedHigh);

    if (triggersScaleOut) {

        int targetCountInstances = 0;

        // Si scale-out se dio por Request High, no usamos formula para calcular cuantas instancias
        // crear, porque necesitariamos saber idealRequestPerVM, que no es fácil. Entonces por simplicidad
        // se añade solo una
        if (reactiveOpinionCpu == ReactiveSignal::Normal){
            int targetCountInstances = totalActualInstances < _maxInstances ? totalActualInstances + 1 : totalActualInstances; // Verificamos que no nos salgamos de _maxInstance
        } else {
            
            // referencia de carga a suplir (usada para calcular ideal de VMs a agregar que suplan necesidad, ya sea actual (por reactive) o futura (por proactive))
            // : la PEOR entre lo observado y lo predicho --
            // si Proactivo predice algo peor que el presente, hay que
            // dimensionar para eso, no solo para el estado actual
            double referenceCpu = currentGlobalCpu.has_value() ? currentGlobalCpu.value() : 0.0;
            if (proactiveOpinion.estimatedValue.has_value()) {
                referenceCpu = max(referenceCpu, *proactiveOpinion.estimatedValue);
            }
    
            int targetCountInstances = CalculateMinimumSafeInstanceCount(totalActualInstances, referenceCpu);
        }

        if (targetCountInstances <= totalActualInstances) {
            // la condicion se disparo, pero la formula dice que la capacidad
            // actual ya alcanza (o ya estamos en el tope permitido) -- no
            // hay una accion real que tomar

            return {scaleAction::Action::Mantain, totalActualInstances, nullopt, "Condicion de scale-out detectada, pero la capacidad actual ya es suficiente o esta en el tope"};
        }

        return {scaleAction::Action::Increment, targetCountInstances, nullopt, "Se cumplió condición para aumentar instancias"};     
    
    };

    // --- Paso 9: SCALE-IN -- AND entre Reactivo y Proactivo ---
    // Unknown cuenta como veto (postura conservadora):
    // solo PredictsSafe deja pasar el scale-in.

    bool triggersScaleIn = reactiveOpinionCpu == ReactiveSignal::SustainedLow && proactiveOpinion.signal == ProactiveSignal::PredictsSafe;

    if (triggersScaleIn) {
        // < solo por salvaguarda, pero actual no deberia ser menor a minInstances
        if (totalActualInstances <= _minInstances) {

            return {scaleAction::Action::Mantain, totalActualInstances, nullopt, "Condicion de scale-in detectada, pero ya se esta en el minimo de instancias permitido"};
        }

        // chequeo fino de seguridad: ¿sigue siendo seguro con UNA instancia
        // menos? -> se usa la misma
        // referencia de carga (peor entre lo actual y lo predicho), pero
        // proyectada contra currentCount-1, no contra currentCount.
        double referenceCpu = currentGlobalCpu.has_value() ? currentGlobalCpu.value() : 0.0;
        if (proactiveOpinion.estimatedValue.has_value()) {
            referenceCpu = max(referenceCpu, *proactiveOpinion.estimatedValue);
        }

        double projectedAtNMinusOne = referenceCpu * totalActualInstances / (totalActualInstances - 1); // Dará mayor a 0 porque totalInstances es > minInstances, puesto que no cayó en primer if

        // Si cpu del sistema, al quitarle una instancia, es mayor a highThershold, no tiene sentido quitarla 
        if (projectedAtNMinusOne > _highThresholdCpu) {
    
            return {scaleAction::Action::Mantain, totalActualInstances, nullopt, "Scale-in vetado: reducir una instancia proyectaria superar el umbral alto, es decir, capacidad resultante no podría soportar carga"};
        }

        // Obtener instancia con menor cpu a eliminar
        // Debemos validar que ValidatedCurrent si tenga datos (en lifeCycle se valida solo history, current no porque solo la queriamos validar si se llegaba a necesitar)

        if (validatedCurrentCpu.status == ValidationStatus::InsufficientData || validatedCurrentCpu.value.empty()) {
            // la formula dice que seria seguro reducir, pero no hay dato
            // confiable para elegir CUAL instancia remover -- mas vale no
            // reducir a ciegas que reducir la equivocada

            return {scaleAction::Action::Mantain, totalActualInstances, nullopt, "Scale-in seria seguro segun la formula, pero no hay dato confiable para elegir que instancia remover"};
        }


        string instanceToRemove = FindLeastLoadedInstance(validatedCurrentCpu.value);

        return {scaleAction::Action::Decrement, totalActualInstances - 1, instanceToRemove,"Se cumplió condición para disminuir instancias" };
    }

    // --- Paso 10: ninguna condicion sostenida se cumplio ---
    return {scaleAction::Action::Mantain, totalActualInstances, nullopt, "Ninguna condición de escalado se cumplió, se mantiene la capacidad actual"};     
}

std::optional<ActionResult> Controller::Act(DecidedAction action){

    // IdToDelete puede ser opcional, pro eso el has_value, porque solo se agrega si accion es decrement
    // De resto, tanto name como TargetCount son obligatorios, siendo
    // Si mantain -> tc = Actual
    // si Reduce -> tc = actual -1
    // si Increment -> tc = calculated
    if (action.action == scaleAction::Action::Decrement && action.idToDelete.has_value()){
        
        return _actioner.ReduceCapacity(action.idToDelete.value());
    }
   
    if (action.action == scaleAction::Action::Increment){
        
        return _actioner.IncreaseCapacity(action.targetCount);
    }

   // action == Mantain -> No se hizo nada
   return nullopt; 
    
}

int Controller::CalculateMinimumSafeInstanceCount(int currentCount, double referenceCpu) const {
    // utilizacion proyectada con N instancias = referenceCpu * currentCount / N
    // (asume que la carga total se mantiene igual y se reparte parejo entre
    // instancias -- el mismo supuesto de round robin ya aceptado para
    // requests). Buscamos el N MINIMO dentro del rango permitido tal que esa
    // utilizacion proyectada quede <= highThreshold -> es decir, minima cantidad de mv que satisfagan carga a suplir
    for (int candidate = _minInstances; candidate <= _maxInstances; ++candidate) {
        double projected = referenceCpu * currentCount / candidate;
        if (projected <= _highThresholdCpu) {
            return candidate;
        }
    }
    // ni con el maximo permitido alcanza -- toca conformarse con el tope
    return _maxInstances;
}

std::string Controller::FindLeastLoadedInstance(const std::unordered_map<std::string, double>& validatedCurrentCpu) const {
    
    // se asume no vacio -- quien llama ya lo verifico antes de invocar esto
    auto it = std::min_element(validatedCurrentCpu.begin(), validatedCurrentCpu.end(),
                                [](const auto& a, const auto& b) { return a.second < b.second; });
    
    return it->first;
}



// Útil porque despues de añadir o quitar instancias, se debe actulizar el _instanceIds
void Controller::SetInstanceIds(vector<string> newIds){
    _instanceIds = move(newIds);
}

