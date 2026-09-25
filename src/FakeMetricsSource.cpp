#include "FakeMetricsSource.h"

#include <algorithm>
#include <fstream>
#include <string>

using namespace std;

// TIPOS
// 
FakeMetricsSource::FakeMetricsSource(vector<string> instanceIds,
                                    double baselineCpuPercent,
                                    double cpuStepStdDev,
                                    double cpuTrendPercentPerMinute,
                                    double baselineRequests,
                                    double requestStepStdDev,
                                    chrono::seconds retentionWindow,
                                    optional<filesystem::path> persistenceDirectory)
    : _instanceIds(move(instanceIds)),
      _baselineCpuPercent(baselineCpuPercent),
      _cpuStepStdDev(cpuStepStdDev),
      _cpuTrendPercentPerMinute(cpuTrendPercentPerMinute),
      _baselineRequests(baselineRequests),
      _requestStepStdDev(requestStepStdDev),
      _retentionWindow(retentionWindow),
      _persistenceDirectory(move(persistenceDirectory)),
      _startTime(chrono::system_clock::now()) {

    if (_persistenceDirectory.has_value()) {
        // si no se puede crear la carpeta, simplemente no se persiste nada 
        error_code ignored; // var para recibir el error, sin lanzar excepción
        filesystem::create_directories(*_persistenceDirectory, ignored);
    }
}

bool FakeMetricsSource::ConsumeFailureFlag() {

    bool shouldFail = _forceNextCallToFail;
    _forceNextCallToFail = false;

    return shouldFail;
}

void FakeMetricsSource::EnsureCpuHistoryUpTo(const string& id, chrono::system_clock::time_point upTo) {

    // en map y unordered_map, ...[clave], obtiene si existe, y si no, crea registro con esa clave
    // y valor por defecto (vacio) -> si no existe, será un metricseries vacio                             
    MetricSeries& series = _cpuHistoryStore[id];  // crea la entrada vacia si es la primera vez que se pide
    // si no, trae historico en RAM de ese id (max 30 min) ->

    if (series.empty()) {
        // primer punto de esta instancia -- arranca en el baseline
        // pasamos timestap y value (que es restringido por clamp) 
        // esta MetricSample le hacemos push_back en values, o sea, en MetricSeries
        series.push_back({_startTime, clamp(_baselineCpuPercent, 0.0, 100.0)});
    }

    // cuanto deberia moverse el "centro" del walk en cada paso por la
    // tendencia configurada, independiente del delta aleatorio
    // es la pendiente de las series de la CPU -> convierte tendencia por minuto, a tendencia 
    // por paso (que por default, paso es 60s, pero alguien lo puede cambiar)
    double trendPerStep = _cpuTrendPercentPerMinute * (chrono::duration<double>(SAMPLE_STEP).count() / 60.0);

    // delta (que es lo que sumamos en cada paso) tendrá desviacion configurada
    normal_distribution<double> delta(0.0, _cpuStepStdDev);

    // extender el walk paso a paso hasta cubrir upTo. Cada paso depende del
    // ANTERIOR YA GUARDADO en `series` -}> un punto que ya existe nunca se
    // recalcula, por eso dos consultas que se sobreponen siempre ven los
    // mismos valores para los instantes que ya se habian generado antes.
    // Como solo se generan los pasos NUEVOS desde la ultima extension (no
    // se recorre todo desde el inicio), el costo amortizado es barato aunque
    // la simulacion lleve corriendo mucho tiempo.
    while (series.back().timestamp < upTo) { // siempre cogemos ultimo dato (que en cada iteración cambiará)
        // .timestamp porque series es una MetricSeries -> vector (id, metricSample) y metricSample tiene
        // dos elementos, timestamp y value 
        auto nextTimestamp = series.back().timestamp + SAMPLE_STEP; // avanzamos paso
        double nextValue = clamp(series.back().value + trendPerStep + delta(_rng), 0.0, 100.0); //generamos siguiente valor, a partir de anterior

        // añadimos MetricSeries
        series.push_back({nextTimestamp, nextValue});
    }

    PruneAndPersist(id, series, upTo);
}

void FakeMetricsSource::EnsureRequestHistoryUpTo(chrono::system_clock::time_point upTo) {
    
    // primer dato -> uso baseline
    if (_requestHistoryStore.empty()) {
        _requestHistoryStore.push_back({_startTime, max(0.0, _baselineRequests)});
    }

    normal_distribution<double> delta(0.0, _requestStepStdDev);
    while (_requestHistoryStore.back().timestamp < upTo) {
        auto nextTimestamp = _requestHistoryStore.back().timestamp + SAMPLE_STEP; // añadimos siguente MetricSample, que sera en timestamp +60, con value x
        double nextValue = max(0.0, _requestHistoryStore.back().value + delta(_rng));

        _requestHistoryStore.push_back({nextTimestamp, nextValue});
    }

    PruneAndPersist("requests", _requestHistoryStore, upTo);
}

FetchResult<vector<string>> FakeMetricsSource::GetInstanceIds() {

    if (ConsumeFailureFlag()) {
        return {FetchStatus::ApiError, {}};
    }

    return {FetchStatus::Ok, _instanceIds};
}

FetchResult<CurrentCpuSnapshot> FakeMetricsSource::GetCurrentCpus(const vector<string>& ids) {
    
    if (ConsumeFailureFlag()) {
        return {FetchStatus::ApiError, {}};
    }

    auto now = chrono::system_clock::now();
    unordered_map<string, double> result;

    for (const auto& id : ids) {
        EnsureCpuHistoryUpTo(id, now); //todos tendran mismo timestamp
        result[id] = _cpuHistoryStore[id].back().value;  // el punto mas reciente ya generado
    }

    return {FetchStatus::Ok, CurrentCpuSnapshot{now, result}};
}

// history de ids
FetchResult<MetricSeriesByInstance> FakeMetricsSource::GetCpuHistory(

    const vector<string>& ids, chrono::seconds window) {
    
    if (ConsumeFailureFlag()) {
        return {FetchStatus::ApiError, {}};
    }

    auto now = chrono::system_clock::now();
    MetricSeriesByInstance result;

    for (const auto& id : ids) {
        EnsureCpuHistoryUpTo(id, now);  // extiende si faltan puntos nuevos, no toca los viejos -> porque vamos hasta now, tienen que estar act

        MetricSeries windowed;
        for (const auto& sample : _cpuHistoryStore[id]) { // aqui no va a leer más de 30 valores -> otra razon para no cargar todo en ram, y hacerlo persistente
            if (sample.timestamp >= now - window && sample.timestamp <= now) { // tomamos samples que esten dentro de window -> desde now - window, hasta now
                windowed.push_back(sample);
            }
        }
        // movemos datos de window a result
        result[id] = move(windowed);
    }

    return {FetchStatus::Ok, result};
}

FetchResult<double> FakeMetricsSource::GetCurrentRequest() {

    if (ConsumeFailureFlag()) {
        return {FetchStatus::ApiError, 0.0};
    }

    auto now = chrono::system_clock::now();
    EnsureRequestHistoryUpTo(now);

    return {FetchStatus::Ok, _requestHistoryStore.back().value};
}

FetchResult<MetricSeries> FakeMetricsSource::GetRequestHistory(chrono::seconds window) {

    if (ConsumeFailureFlag()) {
        return {FetchStatus::ApiError, {}};
    }

    auto now = chrono::system_clock::now();
    EnsureRequestHistoryUpTo(now);

    MetricSeries windowed;

    for (const auto& sample : _requestHistoryStore) {
        if (sample.timestamp >= now - window && sample.timestamp <= now) {
            windowed.push_back(sample);
        }
    }

    return {FetchStatus::Ok, windowed};
}

void FakeMetricsSource::SetCpuOverride(const string& instanceId, double value) {

    auto now = chrono::system_clock::now();
    EnsureCpuHistoryUpTo(instanceId, now);  // asegura que exista al menos un punto que sobreescribir

    _cpuHistoryStore[instanceId].back().value = clamp(value, 0.0, 100.0); //asignamos valor que pasamos, solo que nos aseguramos que este dentro de ranfo
    // el walk continua desde este valor forzado en el proximo paso -- es una
    // inyeccion real dentro de la serie, no un baseline pasivo aparte
}

void FakeMetricsSource::SetNextCallFails(bool shouldFail) {

    _forceNextCallToFail = shouldFail;
}

void FakeMetricsSource::PruneAndPersist(const string& seriesName, MetricSeries& series, chrono::system_clock::time_point now) {
    
    // seriesName podria ser id de instanca, para cpu, o 'requests' para requests
    if (series.empty()) {
        return;
    }

    auto cutoff = now - _retentionWindow; // retorna time_point n segundos antes

    // la serie esta ordenada ascendente (invariante de MetricSeries), asi que
    // encontrar donde cortar es simple: el primer punto
    // que SI se queda es el primero con timestamp >= cutoff
    // lower_bound retorna elemento que no sea menor al valor buscado -> tenemos que decirle, en nuestro tipo de datos, que es <
    auto firstToKeep = lower_bound(
        series.begin(), series.end(), cutoff,
        [](const MetricSample& sample, chrono::system_clock::time_point t) { // hay que poner lambda porque no sabe comparar un metricSample con un time_point
            return sample.timestamp < t; // t sería cutoff
        });

    if (firstToKeep == series.begin()) {
        return;  // nada tan viejo todavia, no hay que podar nada -> el primero a guardar es el primer elemento, entonces para podar (antes de él) no hay nada
    }

    PersistSamplesToPrune(seriesName, series.begin(), firstToKeep);

    series.erase(series.begin(), firstToKeep); // quitamos desde inicial, hasta firsttokeep)

}

void FakeMetricsSource::PersistSamplesToPrune(const string& seriesName, MetricSeries::const_iterator first,
                                            MetricSeries::const_iterator last) { //const porque no va a modificar elementos

    if (_persistenceDirectory.has_value()) {
        
        auto filePath = *_persistenceDirectory / (seriesName + ".csv");
        ofstream file(filePath, ios::app);

        if (file) {
            // best-effort: si por algun motivo no se pudo abrir el archivo,
            // preferimos seguir podando (y perder esos puntos) antes que
            // detener la simulacion por un problema de disco
            for (auto it = first; it != last; ++it) {
                auto epochSeconds =
                    chrono::duration_cast<chrono::seconds>(it->timestamp.time_since_epoch())
                        .count();
                file << epochSeconds << "," << it->value << "\n";
            }
        }
    }
    
    //file se cierra cuando sale del bloque -> Porque ofstream file(...) es un objeto 
    //local, y C++ usa RAII: cuando el objeto sale de su ámbito (scope), se destruye 
    //automáticamente y su destructor cierra el archivo
}