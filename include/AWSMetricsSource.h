#pragma once

#include <aws/autoscaling/AutoScalingClient.h>
#include <aws/monitoring/CloudWatchClient.h>

#include "IMetricsSource.h"

// Implementacion real: habla con Auto Scaling (para listar instancias) y con
// CloudWatch (para las metricas). Se llama AWSMetricsSource

// Manejo de fallos: cada llamada a AWS se reintenta internamente (ver RetryOnFailure
// en el .cpp) antes de devolver FetchStatus::ApiError -- Controller nunca ve
// intentos individuales, solo el resultado final.
class AWSMetricsSource: public IMetricsSource{
public:
    // constructor
    AWSMetricsSource(std::string asgName, std::string targetGroupArn);

    FetchResult<std::vector<std::string>> GetInstanceIds() override;

    //CPU
    FetchResult<CurrentCpuSnapshot> GetCurrentCpus(const std::vector<std::string>& ids) override;

    FetchResult<MetricSeriesByInstance> GetCpuHistory(
        const std::vector<std::string>& ids,
        std::chrono::seconds window) override;

    // Requests
    FetchResult<double> GetCurrentRequest() override;

    FetchResult<MetricSeries> GetRequestHistory(std::chrono::seconds window) override;

private:
    static constexpr int QUERY_PERIOD_SECONDS = 60; // Resolución de cada punto -> AWS captura de forma continua, le pedimos que agrupe por buckets de un minuto, entonces los datos continuos que quedan en ese bucjet, los promedia y ese es el valor que nos da
    // no lo definimos como chrono:second porque API pide int
    static constexpr std::chrono::minutes CURRENT_WINDOW{5}; //Definimos como minutos, para mantener la "unidad" de minuto
    // Es usado para establecer la ventana que solicitaremos en current, porque puede que los datos no esten disponibles inmediatamente -> si pedimos actual -> vacio. Entonces pedimos una ventana de los últimos 5 minutos, y obtenemos el último "punto/bucket"

    std::string _asgName;
    std::string _targetGroupArn;
    Aws::CloudWatch::CloudWatchClient _cloudWatchClient;
    Aws::AutoScaling::AutoScalingClient _autoScalingClient;
};