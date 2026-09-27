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
using namespace Analytics;
using namespace ValidatorUtils;

using json = nlohmann::json;

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
      _highThreshold(_config["highThreshold "].get<double>()),
      _lowThreshold(_config["lowThreshold "].get<double>()),
      _historyWindow(chrono::seconds{stoll(_config["historyWindow"].get<string>())}),
      _horizonWindow(chrono::seconds{stoll(_config["horizonWindow"].get<string>())}),
      _sustainedHighWindow(chrono::seconds{stoll(_config["sustainedHighWindow"].get<string>())}),
      _sustainedLowWindow(chrono::seconds{stoll(_config["sustainedLowWindow"].get<string>())}),
      _queryPeriod(chrono::seconds{stoll(_config["queryPeriod"].get<std::string>())}),
      _scaleOutCooldown(chrono::seconds{stoll(_config["scaleOutCooldown"].get<std::string>())}),
      _scaleInCooldown(chrono::seconds{stoll(_config["scaleInCooldown"].get<std::string>())}),
      _rng(std::random_device{}()),
      _valuePredictor(make_shared<LinearRegressionPredictor>()),
      _validator(_valuePredictor),
      _proactive(_valuePredictor, _horizonWindow, _highThreshold),
      _reactive(_highThreshold, _lowThreshold, _sustainedHighWindow, _sustainedLowWindow) {


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

json LoadConfig(const string& configFile) {
    
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


void Controller::LifeCycle(){
    // obtener ids en primer ciclo -> settea atributo _ids -> si false, return

    // Paso 1: Obtener instancias actuales
    if (!GetInstanceIds()){
        // LogAndMaintain(now, -1, "fallo la consulta de instancias a AWS tras agotar reintentos");
        return;
    }

    // --- Paso 2: caso especial -- 0 instancias no es "no se sabe", es la
    // confirmacion de que no hay capacidad sirviendo nada ---
    if (_instanceIds.empty()){
        // HandleZeroInstances(now);
        return;
    }

    // TODO: 

    // Aquie deberia ir la def de currentCoun -> int currentCount = static_cast<int>(ids.size());
    // si no, seguir para pedir métricas

    auto now = chrono::system_clock::now(); // Tendrá ligera diferencia a generado en getCurrents, pero por eso tenemos un timestampTolerance en demás funciones

    // --- Paso 3: snapshot actual de CPU y Request
    FetchResult<CurrentCpuSnapshot> currentCpus = GetCurrentCpus();

    if (!currentCpus.IsOk()) {
        //LogAndMaintain(now, currentCount, "fallo la consulta de CPU actual a AWS tras agotar reintentos");
        return;
    }

    FetchResult<double> currentRequest = GetCurrentRequest();

    if (!currentRequest.IsOk()) {
        //LogAndMaintain(now, currentCount, "fallo la consulta de Request actual a AWS tras agotar reintentos");
        return;
    }

    // --- Paso 4: historico de CPU y de Request, con ventana suficiente para Reactivo y
    // Proactivo ---

    FetchResult<MetricSeriesByInstance> historyCpus = _metricsSource->GetCpuHistory(_instanceIds, _historyWindow);
    
    if (!historyCpus.IsOk()) {
        // LogAndMaintain(now, currentCount, "fallo la consulta de historico de CPU a AWS tras agotar reintentos");
        return;
    }
    
    FetchResult<MetricSeries> historyRequest = _metricsSource->GetRequestHistory(_historyWindow);
    
    if (!historyRequest.IsOk()) {
        // LogAndMaintain(now, currentCount, "fallo la consulta de historico de Request a AWS tras agotar reintentos");
        return;
    }

    // --- Paso 5: validar el historico completo (rellenar huecos, o
    // descartar el ciclo si falta demasiado) ---

    ValidationResult<MetricSeriesByInstance> validatedCpuHistory = _validator.ValidateCpuHistory(_instanceIds, historyCpus.value, now, _historyWindow, _queryPeriod);
    if (validatedCpuHistory.status == ValidationStatus::InsufficientData) {
        // esto va a pasar seguido en los primeros ciclos del sistema, mientras
        // se acumula suficiente historico --  la respuesta es  mantener y registrar por que
        // LogAndMaintain(now, currentCount,
        //                 "historico de CPU insuficiente para evaluar (posiblemente el sistema esta arrancando)");
        return;
    }

    ValidationResult<MetricSeries> validatedRequestHistory = _validator.ValidateRequestHistory(historyRequest.value, now, _historyWindow, _queryPeriod);
    if (validatedRequestHistory.status == ValidationStatus::InsufficientData) {
        // LogAndMaintain(now, currentCount,
        //                 "historico de Request insuficiente para evaluar (posiblemente el sistema esta arrancando)");
        return;
    }

    // el snapshot actual tambien se valida, pero solo hace falta si mas
    // adelante se decide reducir (para elegir que instancia remover) -- si
    // viene insuficiente, NO se bloquea todo el ciclo aqui; se maneja mas
    // adelante, solo si de verdad se llega a necesitar

    // NOTA: Usamos validatedCpusHistory, y no cpuHistory, porque aprovechamos que ya esta validada
    // y nunca será vacia, porque si llega hasta acá, es porque paso if de insufficientData
    ValidationResult<std::unordered_map<std::string, double>> validatedCurrentCpu = _validator.ValidateCurrentCpus(
        _instanceIds, currentCpus.value.valuesByInstance, validatedCpuHistory.value,
        currentCpus.value.timestamp);
    
    // --- Paso 6: reducir el historico de CPU por instancia a UNA serie global (p95) ---
    // solo CPU, porque Request ya vienen promediados

    // El expectedTimestamp  que espera CalculatGlobalSeries debe coincidir con los que se "creó" el HistoryCpus
    vector<chrono::system_clock::time_point> expectedTimestamps = ValidatorUtils::BuildExpectedTimestamps(now, _historyWindow, _queryPeriod);
    MetricSeries globalCpusHistory = CalculateGlobalCpuSeries(validatedCpuHistory.value, expectedTimestamps);

    // Evaluate 
}


void GetEvaluate(const MetricSeries& globalCpuHistory,
                std::chrono::system_clock::time_point now){

    // Proactive
    
}


// Útil porque despues de añadir o quitar instancias, se debe actulizar el _instanceIds
void Controller::SetInstanceIds(vector<string> newIds){
    _instanceIds = move(newIds);
}

