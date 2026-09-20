#pragma once

#include "IMetricsSource.h"
#include <random>

// Genera metricas simuladas alrededor de un valor base, con ruido gaussiano.
// Como en el Fake todas las instancias se generan con el mismo reloj interno,
// no sufre el problema de timestamps desalineados que sí maneja y soluciona AWS.
//
// SetCpuOverride() y SetNextCallFails() NO estan en IMetricsSource a proposito:
// son utilidades solo para configurar el escenario de prueba antes de pasar el
// objeto como IMetricsSource& al codigo que se esta probando -- por eso hay que
// sostener FakeMetricsSource por su tipo concreto mientras se configura.
class FakeMetricsSource : public IMetricsSource {
public:
    //explicit -> Evita ciertas conversiones implícitas mediante el constructor.
    explicit FakeMetricsSource(std::vector<std::string> instanceIds,
                                double baselineCpuPercent = 40.0,
                                double baselineRequests = 100.0);

    // baseline.. no es el promedio, es el valor central de la distribución con la que se genera ruido gaussiano para cada instancia por separado.                               

    FetchResult<std::vector<std::string>> GetInstanceIds() override;

    FetchResult<std::unordered_map<std::string, double>> GetCurrentCpus(const std::vector<std::string>& ids) override;
    FetchResult<MetricSeriesByInstance> GetCpuHistory(
        const std::vector<std::string>& ids,
        std::chrono::seconds window) override;

    FetchResult<double> GetCurrentRequest() override;
    FetchResult<MetricSeries> GetRequestHistory(std::chrono::seconds window) override;

    // Fuerza el valor de CPU de una instancia especifica (ej. para simular
    // un escenario de scale-out sin esperar a que el ruido aleatorio lo produzca).
    void SetCpuOverride(const std::string& instanceId, double value);
    void SetRequestOverride(const std::string& instanceId, double value);


    // Hace que la proxima llamada (cualquiera de las de arriba) devuelva
    // FetchStatus::ApiError, para probar el manejo de fallos de Controller
    // sin depender de que AWS realmente falle. Se consume una sola vez.
    void SetNextCallFails(bool shouldFail);
    // Modifica valor de atributo _forceNextCallToFail

private:
    static constexpr std::chrono::seconds SAMPLE_STEP{60};

    bool ConsumeFailureFlag();

    std::vector<std::string> _instanceIds;
    double _baselineCpuPercent;
    double _baselineRequests;

    std::unordered_map<std::string, double> _cpuOverrides;

    // generador de numeros aleaotorios
    std::mt19937 _rng;
    bool _forceNextCallToFail = false;
};