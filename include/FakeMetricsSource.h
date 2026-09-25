#pragma once

#include "IMetricsSource.h"

#include <filesystem>
#include <optional>
#include <string>
#include <random>

// ---- TIPOS
// struct MetricSample {std::chrono::system_clock::time_point timestamp;
//   double value; // puede representar un procentaje (cpu) o un numero entero (requests promedio -> 1000 -> 1000 + 1000 + 1000 / 3)

// MetricSeries = std::vector<MetricSample>;
// MetricSeriesByInstance = std::unordered_map<std::string, MetricSeries>

// Genera metricas simuladas como un RANDOM WALK: cada punto nuevo es el
// anterior + un delta aleatorio (mas una tendencia opcional), no ruido
// independiente alrededor de un baseline fijo. Esto representa trafico
// real mejor que una distribucion normal plana alrededor de un centro.
//
// El historial EN MEMORIA es persistente mientras esta dentro de la ventana
// de retencion: una vez generado un punto, se guarda tal cual y nunca se
// recalcula (necesario para que dos consultas que se sobreponen vean los
// mismos valores). Los puntos que salen de la ventana de retencion se podan
// de memoria, pero antes se escriben a disco -- no se pierden, solo dejan de
// vivir en RAM.
class FakeMetricsSource : public IMetricsSource {
public:
    // baselineCpuPercent: valor inicial del walk de CPU (una sola vez, al
    //   generar el primer punto de cada instancia).

    // cpuStepStdDev: que tanto puede moverse el CPU de un paso al siguiente
    //   (desviacion estandar del delta aleatorio por minuto) -> = 15 -> ant +-15

    // cpuTrendPercentPerMinute: deriva adicional por minuto, encima del
    //   delta aleatorio -- para poder simular una subida/bajada real y
    //   probar que el Predictor la detecta.

    // baselineRequests/requestStepStdDev: lo mismo, para el agregado de
    //   requests (una sola serie, no por instancia).

    // retentionWindow: cuanto historial se mantiene EN MEMORIA -- debe ser
    //   mayor que cualquier ventana que el sistema vaya a pedir de verdad
    //   (lowWindow de Reactivo, el historyWindow de Controller, etc), o se 
    //   recibirá menos puntos de los esperados sin ningun error explicito.

    // persistenceDirectory: carpeta donde se archivan los puntos podados,
    //   uno por instancia mas uno para requests. std::nullopt desactiva la
    //   persistencia por completo (los puntos podados simplemente se
    //   descartan) -- util para tests 

    explicit FakeMetricsSource(
        std::vector<std::string> instanceIds,
        double baselineCpuPercent = 40.0,
        double cpuStepStdDev = 15.0, 
        double cpuTrendPercentPerMinute = 0.0,
        double baselineRequests = 100.0,
        double requestStepStdDev = 40.0,
        std::chrono::seconds retentionWindow = std::chrono::minutes(30),
        std::optional<std::filesystem::path> persistenceDirectory = std::filesystem::path("fake_metrics_archive"));

    FetchResult<std::vector<std::string>> GetInstanceIds() override;

    FetchResult<CurrentCpuSnapshot> GetCurrentCpus(
        const std::vector<std::string>& ids) override;
    FetchResult<MetricSeriesByInstance> GetCpuHistory(
        const std::vector<std::string>& ids,
        std::chrono::seconds window) override;

    FetchResult<double> GetCurrentRequest() override;
    FetchResult<MetricSeries> GetRequestHistory(std::chrono::seconds window) override;

    // Inyecta un "shock" en el walk de esta instancia AHORA MISMO (ej. para
    // forzar un escenario de scale-out sin esperar a que el random walk
    // llegue solo). El walk sigue avanzando desde este valor en el proximo
    // paso -- no es un baseline pasivo, es un punto real de la serie.
    void SetCpuOverride(const std::string& instanceId, double value);

    // Hace que la proxima llamada (cualquiera de las de arriba) devuelva
    // FetchStatus::ApiError, para probar el manejo de fallos de Controller
    // sin depender de que AWS realmente falle. Se consume una sola vez.
    void SetNextCallFails(bool shouldFail);

private:
    // Es duración de bucket
    static constexpr std::chrono::seconds SAMPLE_STEP{60};

    bool ConsumeFailureFlag();

    // Extiende _cpuHistoryStore[id] con pasos de random walk hasta cubrir
    // `upTo`. NUNCA recalcula un punto que ya existe -- solo agrega los que
    // faltan, cada uno a partir del ultimo ya guardado.
    void EnsureCpuHistoryUpTo(const std::string& id, std::chrono::system_clock::time_point upTo);

    // Lo mismo, para el agregado de requests.
    void EnsureRequestHistoryUpTo(std::chrono::system_clock::time_point upTo);
    

    void PersistSamplesToPrune(const std::string& seriesName, MetricSeries::const_iterator first,
                                MetricSeries::const_iterator last);

    // Poda de `series` los puntos mas viejos que (now - _retentionWindow).
    // Antes de descartarlos de memoria, si hay carpeta de persistencia
    // configurada, los escribe a disco -- append, nunca sobreescribe.
    // seriesName se usa como nombre de archivo (el instance id, o
    // "requests").
    void PruneAndPersist(const std::string& seriesName, MetricSeries& series,
                          std::chrono::system_clock::time_point now);

    std::vector<std::string> _instanceIds;
    double _baselineCpuPercent;
    double _cpuStepStdDev;
    double _cpuTrendPercentPerMinute;
    double _baselineRequests;
    double _requestStepStdDev;
    std::chrono::seconds _retentionWindow;
    std::optional<std::filesystem::path> _persistenceDirectory;

    // historial en memoria -- persistente mientras esta dentro de la
    // ventana de retencion; lo que sale de esa ventana se poda (y se
    // archiva a disco si hay carpeta configurada)
    
    MetricSeriesByInstance _cpuHistoryStore;
    MetricSeries _requestHistoryStore;

    std::chrono::system_clock::time_point _startTime; // tiempo en que se inicializa el objeto 
    // -> sirve para timestamp de primer dato

    // aqui SI tiene sentido que _rng tenga memoria (mantiene misma entropia): cada paso
    // del walk se genera UNA sola vez (al extender el historial), nunca se vuelve a
    // tirar el dado para un paso que ya existe
    std::mt19937 _rng{std::random_device{}()};
    // Proporciona aleatoriedad para delta -> manteniendo seed, aseguramos que 
    // los estados del motor de aleatoriedad de forma ordenada

    bool _forceNextCallToFail = false;
};