#include "AWSMetricsSource.h"
 
#include <aws/autoscaling/model/DescribeAutoScalingGroupsRequest.h>
#include <aws/monitoring/model/Dimension.h>
#include <aws/monitoring/model/GetMetricDataRequest.h>
#include <aws/monitoring/model/ScanBy.h>
#include <algorithm>
#include <thread>

using namespace std;

//namespace anónimo -> Su propósito en este caso es hacer que QueryIdForIndex sea privada de este archivo .cpp.
// Lo declarado dentro de namespace son cosas que NO pertenecen a la clase, sino al .cpp, entonces, 
//ni retryonfailures ni queryidforindex pertencen a la clase
// y por eso, constantes no necesitan static
namespace {
    // en namspc y no en .h porque -> ocultar detalles de implementación que otros componentes no necesitan conocer.
    constexpr int MAX_RETRIES = 2;  // reintentos ADICIONALES tras el primer intento (3 llamadas en total)
    constexpr chrono::milliseconds RETRY_BACKOFF{200}; // tiempo que esperamos entre reintentos

    // Reintenta `attempt` hasta MAX_RETRIES veces si no llega Ok. Encapsula la
    // politica de reintentos en un solo lugar -- Controller nunca ve intentos
    // individuales, solo el resultado final (Ok, o ApiError ya con los reintentos agotados).

    // recibe diferentes tipos de funciones (Fn)
    template <typename Fn>

    // funcion normal es: retorno_t nombre(param) {}
    // acá usamos -> para indicar tipo de retorno despues (porque este depende de attemp, que se define en paramts)
    // con auto, indicamos que tipo de retorno se indicará despues, con ->
    // decltype indica el tipo de lo que retorne la funcion attempt al ejecutarla (pero no la ejecuta)

    auto RetryOnFailure(Fn&& attempt) -> decltype(attempt()) {  //Esta función recibe una función que hace la llamada a AWS, la ejecuta, y si falla la vuelve a intentar hasta MAX_RETRIES veces.
        //Fn%% permite referenciar los dos tipos de expresiones, lvalue y rvalue
        // o sea, recibe funcion_a, donde esta es la var de una funcion lambda (lvalue), o la misma funcion lambda (rvalue)
        decltype(attempt()) result;
        for (int tries = 0; tries <= MAX_RETRIES; ++tries) {
            result = attempt();

            if (result.status == FetchStatus::Ok) {
                return result;
            }
            if (tries < MAX_RETRIES) {
                this_thread::sleep_for(RETRY_BACKOFF * (tries + 1)); // porque si error en temp en aws, esperamos un poco 
                //antes de reintentar
            }
        }
        return result;  // si status sigue en ApiError -- Controller decide que hacer (ver GetCurrentCpu)
    }


    // El Id de cada MetricDataQuery debe empezar con minuscula y ser alfanumerico --
    // no podemos usar el instance id tal cual (tiene guiones), asi que mapeamos por indice.
    //size_t porque será positivo
    string QueryIdForIndex(size_t index) {
        return "id" + to_string(index);
    }
} 

// con move decimos: Construye _nombre usando los recursos que actualmente tiene nombre, en vez de hacer una copia.
// implementamos constructor, recibe 2 param, y los "transfiere" a los atributos del objeto.
AWSMetricsSource::AWSMetricsSource(string asgName, string targetGroupArn):
    _asgName(move(asgName)), _targetGroupArn(move(targetGroupArn)) {}
    

FetchResult<vector<string>> AWSMetricsSource::GetInstanceIds() {
     // lambda => [capturas](parámetros) -> tipo_de_retorno { ..cuerpo.. }
    // [this] -> "quiero que este lambda pueda acceder al this del objeto actual". POr eso podemos usar param de clase.
    return RetryOnFailure([this]() -> FetchResult<vector<string>> { 

        // creamos objeto para realizar petición a AWS para describe..., y luego le añadimos nombre 
        // a request, de asg
        // cuando la ejecutemos, dirá "AWS, dame información del ASG llamado my-web-asg."
        Aws::AutoScaling::Model::DescribeAutoScalingGroupsRequest request;
        request.AddAutoScalingGroupNames(_asgName); 
        
        // ejecutamos petición, usando cliente de as
        // DescribeAutoScalingGroups manda la solucitid/request
        auto outcome = _autoScalingClient.DescribeAutoScalingGroups(request);      
        if (!outcome.IsSuccess()) {
            // TODO: registrar outcome.GetError() en el log de auditoria del ciclo
            // cerr << "Error with AutoScaling::DescribeAutoScalingGroups. "
            //     << outcome.GetError().GetMessage()
            //     << endl;
            return {FetchStatus::ApiError, {}}; 
        }

        vector<string> ids;
        // obtenemos vector con groups solicitados -> como pasamos name, solo retornará vector con 1 elemento 
        // retorna vector porque el parámetro acepta varios nombres, no solo uno
        const auto& groups = outcome.GetResult().GetAutoScalingGroups();
        if (groups.empty()) {
            // TODO: registrar que groups de asg es vacio en el log de auditoria del ciclo
            // el ASG no aparece en la respuesta -- exitosa, pero vacia de verdad
            return {FetchStatus::Ok, ids};
        }
        
        // Obtenemos grupo único (un Auto scailin GROUP)-> y obtenemos sus instancias
        for (const auto& instance : groups.front().GetInstances()) {
            // solo instancias que ya sirven trafico real (excluye Pending/Terminating,
            // que contaminarian el p95 con 0% de CPU sin estar realmente disponibles)
            if (instance.GetLifecycleState() == Aws::AutoScaling::Model::LifecycleState::InService) {
                ids.push_back(instance.GetInstanceId());
            }
        }

        return {FetchStatus::Ok, ids};
    });  
}

// Hay que decir IMetricsSource pq ya no está dentro de scope, en el .h sí Dentro de la definición de la clase derivada, C++ permite 
// acceder a tipos heredados directamente, pero en el .cpp ya no.
FetchResult<MetricSeriesByInstance> AWSMetricsSource::GetCpuHistory(const vector<string>& ids, chrono::seconds window) {
    
    return RetryOnFailure([this, &ids, window]() -> FetchResult<MetricSeriesByInstance> { 
    
        MetricSeriesByInstance result;
    
        if (ids.empty()) {
            return {FetchStatus::Ok, result}; //porque no fue fallo de api, solo pasaron ids vacios
        }
        
        Aws::CloudWatch::Model::GetMetricDataRequest request; // obtenemos objeto tipo request
    
        auto now = chrono::system_clock::now(); 
        request.SetStartTime(Aws::Utils::DateTime(now - window)); // window porque será history
        request.SetEndTime(Aws::Utils::DateTime(now));
        // el default de CloudWatch es TimestampDescending (mas nuevo primero) --
        // lo forzamos a ascendente para cumplir la invariante de MetricSeries
        request.SetScanBy(Aws::CloudWatch::Model::ScanBy::TimestampAscending);

        // 1. Para cada instancia, construimos DataMetricquery
        for (size_t i = 0; i < ids.size(); ++i) {
            Aws::CloudWatch::Model::Metric metric; // objeto que representará que metrica queremos consultar
    
            metric.SetNamespace("AWS/EC2");
            metric.SetMetricName("CPUUtilization");
            // especificamos instancia de la que queremos la métrica
            //Una dimension sirve para especificar sobre qué recurso quieres la métrica.
            metric.AddDimensions(Aws::CloudWatch::Model::Dimension().WithName("InstanceId").WithValue(ids[i]));
        
            // Metrica contiene namespace, nombre metrica, y de qué instancia la queremos
    
            //objeto que describe cómo quieres consultar/agrupar esa métrica.
            Aws::CloudWatch::Model::MetricStat stat;
            stat.SetMetric(metric); // metrica que creamos
            stat.SetPeriod(QUERY_PERIOD_SECONDS); // resolución temporal de los datos que queremos
            stat.SetStat("Average"); // método estadístico que quieres utilizar.
        
            // creamos objeto consulta, que será el enviado a api
            Aws::CloudWatch::Model::MetricDataQuery query;
            query.SetId(QueryIdForIndex(i)); //id de query, no de instancia
            query.SetMetricStat(stat); // Esta query debe utilizar el MetricStat que acabamos de construir.
            
            //Agregamos query a petición (tendrá n queries, una por cada instancia)
            request.AddMetricDataQueries(query); 
        }
        
        //enviamos petición a cloudwatch
        auto outcome = _cloudWatchClient.GetMetricData(request);
    
        if (!outcome.IsSuccess()) {
            // TODO: registrar outcome.GetError();
            return {FetchStatus::ApiError, {}};
        }
        
        // 2. Construimos metricSeries para cada instancia
    
        //recorremos cada resultado (por instancia) -> nos da los n resgistros de CPU en el timest.
        // ejm: id1 -> timestamp = [..], y values = [..]
        for (const auto& metricResult : outcome.GetResult().GetMetricDataResults()) {
            // recuperamos a que instancia corresponde este resultado por el indice
            // codificado en el Id ("id3" -> indice 3)  -> con substr quitamos dos primeros chars
            // stoul() convierte una cadena de texto en un número entero largo sin signo
            size_t index = stoul(metricResult.GetId().substr(2));
            const string& instanceId = ids[index];
        
            // vector con cpus de una instancia con timestamps
            MetricSeries series;
    
            // obtenemos los n resultados que nos dio metricResult -> dos listas:
            // timestamps:[10:00, 10:01, 10:02] y values: [23.5, 27.1, 31.4]
    
            const auto& timestamps = metricResult.GetTimestamps();
            const auto& values = metricResult.GetValues();
    
            for (size_t j = 0; j < timestamps.size(); ++j) {
                // generamos "tupla" (timest., value) de cada elemento j -> (10:00, 23.5)
                series.push_back({timestamps[j].UnderlyingTimestamp(), values[j]});
            }
            // move permite mover el vector dentro del unordered_map en lugar de copiar todas sus muestras.
            result[instanceId] = move(series);
        }
    
         return {FetchStatus::Ok, result};
        // result -> 
        //{ status: Ok
        // result = 
        // "i-AAA" → [{10:00, 20}, {10:01, 25}, {10:02, 30}]
        // "i-BBB" → [{10:00, 40}, {10:01, 35}, {10:02, 38}]}
    });
}

FetchResult<CurrentCpuSnapshot> AWSMetricsSource::GetCurrentCpus(
    const vector<string>& ids) {

    auto history = GetCpuHistory(ids, CURRENT_WINDOW); //ya acá hicimos el retry, por eso no se pone en esta func
    //history es de tipo fetchresult -> tiene fetchstatus y value
    if (!history.IsOk()) {
        return {FetchStatus::ApiError, {}};
    }

    // Distintas instancias pueden tener su ultimo punto disponible en timestamps
    // distintos (retraso de reporte, instancia recien creada, etc), AUNQUE los
    // buckets de la consulta sean los mismos para todas. No podemos comparar
    // "el ultimo de cada una" como si fueran el mismo instante: primero hay que
    // decidir CUAL instante es "el actual" para este ciclo (el que comparte la
    // mayoria -- moda, no maximo, para no dejarnos guiar por una sola instancia
    // adelantada), y luego leer cada instancia justo en ese instante.

    // Obtenemos cant de instancias por último timestamp
    // no usamos unordered_map pq tipo es time_point, y no hay funcion hash que nos den para es tipo
    // debemos de crearla nosotros -> y como los timep_point tienen un orden >, podemos usar map, que
    // no necesita hash
    map<chrono::system_clock::time_point, int> votes;
    for (const auto& [id, series] : history.value) {
        if (!series.empty()) {
            // sumamos 1 en el timestamp que propone (o sea, vamos contando cantidad de últimos timestamp)
            votes[series.back().timestamp]++;
        }
    }

    //obtenemos timestamp media 
    chrono::system_clock::time_point targetTimestamp{};
    int bestVotes = 0;
    for (const auto& [timestamp, count] : votes) {
        // actualizamos target y bestvotes si count tiene mas cant que bestvotes, o tienen misma pero con un timestamp mayor
        if (count > bestVotes || (count == bestVotes && timestamp > targetTimestamp)) {
            targetTimestamp = timestamp;
            bestVotes = count;
        }
    }

    //current es mapa instanceId -> cpu
    unordered_map<string, double> current;
    for (const auto& [id, series] : history.value) {
        // [idA, [10:11, 10:12...]]

        // find_if retorna iterador que apunta a objeto en iterable que cumpla una condición
        // con .begin y con .end, le decimos desde donde hasta donde iterar (sobre todos los elementos de series)
        // y le aplciamos lambda. COn [&] decimos que puede usar elementos externos de lambda
        // por referencia, por eso podemos usar target..
        // si encuentra elemento, iterator queda apuntando a primero que lo haga
        // si no, queda apuntando a series.end, que es un elemento vacio después del ultimo elemento
        auto itetator = find_if(series.begin(), series.end(), [&](const MetricSample& sample) {
            return sample.timestamp == targetTimestamp;
        });
        if (itetator != series.end()) {
            current[id] = itetator->value;
        }
        // si esta instancia no tiene dato en targetTimestamp, queda fuera del mapa
        // a proposito -- que tan grave es esa ausencia (y si vale la pena pedirle
        // un valor al Predictor) lo decide el Validator, no esta funcion
    }

    // el timestamp elegido (la moda) viaja empaquetado junto al mapa -- sin
    // esto, Controller no tendria forma de saber a que instante corresponde
    // este snapshot, y no podria pasarselo despues a Validator::ValidateCurrentCpu
    return {FetchStatus::Ok, CurrentCpuSnapshot{targetTimestamp, current}};
}

FetchResult<MetricSeries> AWSMetricsSource::GetRequestHistory(chrono::seconds window) {

    return RetryOnFailure([this, window]() -> FetchResult<MetricSeries> {

    });
    // Generamos metrica con sus datos y sobre quién la ejecitaremos
    Aws::CloudWatch::Model::Metric metric;
    metric.SetNamespace("AWS/ApplicationELB");
    metric.SetMetricName("RequestCountPerTarget");
    metric.AddDimensions(Aws::CloudWatch::Model::Dimension().WithName("TargetGroup").WithValue(_targetGroupArn));
 
    // Describe cómo quieres consultar/agrupar esa métrica.
    Aws::CloudWatch::Model::MetricStat stat;
    stat.SetMetric(metric);
    stat.SetPeriod(QUERY_PERIOD_SECONDS);
    stat.SetStat("Average");
    
    // Creamos query con id de query request poruqe es la unica query que habrá
    Aws::CloudWatch::Model::MetricDataQuery query;
    query.SetId("requests");
    query.SetMetricStat(stat);
 
    // Creamos request con query, y con los tiempos start y end
    Aws::CloudWatch::Model::GetMetricDataRequest request;

    auto now = chrono::system_clock::now();
    request.SetStartTime(Aws::Utils::DateTime(now - window));
    request.SetEndTime(Aws::Utils::DateTime(now));
    request.SetScanBy(Aws::CloudWatch::Model::ScanBy::TimestampAscending);
    request.AddMetricDataQueries(query);
 

    MetricSeries series;
    auto outcome = _cloudWatchClient.GetMetricData(request);
    if (!outcome.IsSuccess()){
        return {FetchStatus::ApiError, {}};
    }
    
    if (outcome.GetResult().GetMetricDataResults().empty()) {
        return {FetchStatus::Ok, {}};  // llamada exitosa, simplemente sin datos en la ventana
    }

    // metricResult contiene varios timestamp y values, pero solo un único "id", porque 
    // RequestCountPerTarget da promedio -> acá hay n timestamps, y cada uno trae el value promedio de todas
    // las instancias
    const auto& metricResult = outcome.GetResult().GetMetricDataResults().front();
    const auto& timestamps = metricResult.GetTimestamps();
    const auto& values = metricResult.GetValues();

    for (size_t j = 0; j < timestamps.size(); ++j) {
        // timestamps son de tipo aws::utils::datetime -> obtenemos el timestamp interno que contienen
        // para que coincida con tipos en metricsample, que es lo que pusheamos a series
        series.push_back({timestamps[j].UnderlyingTimestamp(), values[j]});
    }
    
    return {FetchStatus::Ok, series};
}
 
FetchResult<double> AWSMetricsSource::GetCurrentRequest() {
    auto history = GetRequestHistory(CURRENT_WINDOW);
    // ya se hicieron los retry en history, y retorna objeto fetchresult<...> -> tenemos un status
    // y un value

    if (!history.IsOk()) {
        return {FetchStatus::ApiError, 0.0};
    }
    if (history.value.empty()) {
        return {FetchStatus::Ok, 0.0};  // exito, pero sin trafico reportado en la ventana
    }
    return {FetchStatus::Ok, history.value.back().value};
}