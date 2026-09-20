#include "AWSMetricsSource.h"
 
#include <aws/autoscaling/model/DescribeAutoScalingGroupsRequest.h>
#include <aws/monitoring/model/Dimension.h>
#include <aws/monitoring/model/GetMetricDataRequest.h>
//#include <utility>

using namespace std;

//namespace anónimo -> Su propósito en este caso es hacer que QueryIdForIndex sea privada de este archivo .cpp.
namespace {
// El Id de cada MetricDataQuery debe empezar con minuscula y ser alfanumerico --
// no podemos usar el instance id tal cual (tiene guiones), asi que mapeamos por indice.
//size_t porque será positivo
    std::string QueryIdForIndex(size_t index) {
        return "id" + std::to_string(index);
    }
} 

// con move decimos: Construye _nombre usando los recursos que actualmente tiene nombre, en vez de hacer una copia.
// implementamos constructor, recibe 2 param, y los "transfiere" a los atributos del objeto.
AWSMetricsSource::AWSMetricsSource(string asgName, string targetGroupArn):
    _asgName(move(asgName)), _targetGroupArn(move(targetGroupArn)) {}
    

vector<string> AWSMetricsSource::GetInstanceIds() {
    // creamos objeto para realizar petición a AWS para describe..., y luego le añadimos nombre 
    // a request, de asg
    // cuando la ejecutemos, dirá "AWS, dame información del ASG llamado my-web-asg."
    Aws::AutoScaling::Model::DescribeAutoScalingGroupsRequest request;
    request.AddAutoScalingGroupNames(_asgName); 

    // ejecutamos petición, usando cliente de as
    // DescribeAutoScalingGroups manda la solucitid/request
    auto outcome = _autoScalingClient.DescribeAutoScalingGroups(request);

    vector<string> ids;
    if (!outcome.IsSuccess()) {
        // TODO: registrar outcome.GetError() en el log de auditoria del ciclo
        // std::cerr << "Error with AutoScaling::DescribeAutoScalingGroups. "
        //     << outcome.GetError().GetMessage()
        //     << std::endl;

        return ids;
    }
 
    // obtenemos vector con groups solicitados -> como pasamos name, solo retornará vector con 1 elemento 
    // retorna vector porque el parámetro acepta varios nombres, no solo uno
    const auto& groups = outcome.GetResult().GetAutoScalingGroups();
    if (groups.empty()) {
        // TODO: registrar que groups de asg es vacio en el log de auditoria del ciclo
        return ids;
    }
 
    // Obtenemos grupo único (un Auto scailin GROUP)-> y obtenemos sus instancias
    for (const auto& instance : groups.front().GetInstances()) {
        // solo instancias que ya sirven trafico real (excluye Pending/Terminating,
        // que contaminarian el p95 con 0% de CPU sin estar realmente disponibles)
        if (instance.GetLifecycleState() == Aws::AutoScaling::Model::LifecycleState::InService) {
            ids.push_back(instance.GetInstanceId());
        }
    }

    return ids;
}

// Hay que decir IMetricsSource pq ya no está dentro de scope, en el .h sí Dentro de la definición de la clase derivada, C++ permite 
// acceder a tipos heredados directamente, pero en el .cpp ya no.
IMetricsSource::MetricSeriesByInstance AWSMetricsSource::GetCpuHistory(const vector<string>& ids, chrono::seconds window) {
    
    MetricSeriesByInstance result;

    if (ids.empty()) {
        return result;
    }
 
    Aws::CloudWatch::Model::GetMetricDataRequest request; // obtenemos objeto tipo request

    auto now = std::chrono::system_clock::now(); 
    request.SetStartTime(Aws::Utils::DateTime(now - window)); // window porque será history
    request.SetEndTime(Aws::Utils::DateTime(now));
 
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
        // TODO: registrar outcome.GetError(); el Controller vera un mapa vacio
        // y debe tratarlo como "metrica no disponible este ciclo", no como CPU en 0%
        return result;
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

    return result;
    // result
    // "i-AAA" → [{10:00, 20}, {10:01, 25}, {10:02, 30}]
    // "i-BBB" → [{10:00, 40}, {10:01, 35}, {10:02, 38}]
}
 
unordered_map<string, double> AWSMetricsSource::GetCurrentCpus(const std::vector<std::string>& ids) {

    // reutiliza GetCpuHistory con una ventana corta en vez de duplicar la
    // construccion de la query -- una sola fuente de verdad para "como se arma
    // una consulta de CPU a CloudWatch"
    auto history = GetCpuHistory(ids, CURRENT_WINDOW); // obtiene, máximo, current_window muestras en values por id -> por eso history, proque no tiene solo un timest.
 
    std::unordered_map<std::string, double> current; // contendrpa id de cpu, junto con su valor

    // como history es  -> string, lista de metricas (MetricSeries)
    for (const auto& [instanceId, series] : history) {
        if (!series.empty()) {
            current[instanceId] = series.back().value;  // el mas reciente -> o sea, el último en la lista
            //TODO: si dos instancias tienen timestsmp doferente, proque a una le llego y a otra no, etc. 
            // hace que compararlas con p95 no sea lo más útil -> de pronto acá, antes de guardarlo en 
            //current[insranceid], guardarlo en tupla con su timestamp
            // cuando se haya procesado todo, obtenemos timestamp y si alguno es menor al máxi, lanzamos
            //prediccion con modelo para ese timestamp (no horizonte de tiempo)
        }
        // TODO: si serie está vacia -> hubo error -> usar predictor?
        // si series esta vacia, la instancia queda fuera del mapa a proposito
    }

    return current;
}
 
IMetricsSource::MetricSeries AWSMetricsSource::GetRequestHistory(chrono::seconds window) {

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

    auto now = std::chrono::system_clock::now();
    request.SetStartTime(Aws::Utils::DateTime(now - window));
    request.SetEndTime(Aws::Utils::DateTime(now));
    request.AddMetricDataQueries(query);
 

    MetricSeries series;
    auto outcome = _cloudWatchClient.GetMetricData(request);
    if (!outcome.IsSuccess() || outcome.GetResult().GetMetricDataResults().empty()) {
        return series;  // vacio -> el Controller lo trata como metrica faltante
    }
    
    // metricResult contiene varios timestamp y values, pero solo un único "id", porque 
    // RequestCountPerTarget da promedio -> acá hay n timestamps, y cada uno trae el value promedio de todas
    // las instancias
    const auto& metricResult = outcome.GetResult().GetMetricDataResults().front();
    const auto& timestamps = metricResult.GetTimestamps();
    const auto& values = metricResult.GetValues();

    for (size_t j = 0; j < timestamps.size(); ++j) {
        series.push_back({timestamps[j].UnderlyingTimestamp(), values[j]});
    }
    return series;
}
 
double AWSMetricsSource::GetCurrentRequest() {
    auto history = GetRequestHistory(CURRENT_WINDOW);
    // OJO: 0.0 aqui es ambiguo entre "sin trafico real" y "la consulta fallo".
    // Si el Controller necesita distinguirlos, deberia llamar GetRequestHistory
    // directamente y revisar si el vector vino vacio, en vez de confiar en este double.
   
    // TODO: si serie está vacia -> hubo error o request 0? -> usar predictor?
    //de pronto retornar con código de estado
   
    return history.empty() ? 0.0 : history.back().value;
}