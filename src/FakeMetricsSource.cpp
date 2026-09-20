#include "FakeMetricsSource.h"

#include <algorithm>
using namespace std;

// lo que hay despues de : es la lista de incialización
// _base..(base) -> copia contenido a _base..
FakeMetricsSource::FakeMetricsSource(vector<string> instanceIds, double baselineCpuPercent,
double baselineRequests)
    : _instanceIds(move(instanceIds)),
      _baselineCpuPercent(baselineCpuPercent),
      _baselineRequests(baselineRequests),
      _rng(random_device{}()) {}
// random_device{} crea objeto capaz de obtener una semilla no determinista. ->
// con () llamamos ese objeto, para obtenerla
// y lo asignamos a _rng -> random number generator

// Con esta, consumimos nextcalltofail -> esta debe estar en true (con setnextcallfails())
// y luego de consumirla, esta se pone otra vez como false -> se consume
bool FakeMetricsSource::ConsumeFailureFlag() {
    bool shouldFail = _forceNextCallToFail;
    _forceNextCallToFail = false;
    return shouldFail;
}

FetchResult<vector<string>> FakeMetricsSource::GetInstanceIds() {
    // Retornamos error de Api
    if (ConsumeFailureFlag()) {
        return {FetchStatus::ApiError, {}};
    }
    // ok, y damos instanceids que ya nos dieron en el constructor
    return {FetchStatus::Ok, _instanceIds};
}

FetchResult<unordered_map<string, double>> FakeMetricsSource::GetCurrentCpus(
    const vector<string>& ids) {
        
    if (ConsumeFailureFlag()) {
        return {FetchStatus::ApiError, {}};
    }

    // result contendrá map (dict) (id: value)
    unordered_map<string, double> result;
    normal_distribution<double> noise(0.0, 5.0); // crea ruido
    // objeto que genera una distribución normal (campana) con media 0, y desviación estandar 5
    // es el encargado de darle una distribución normal a los numero aleatorios que genera rng
    // Los valores cerca de la media son los más frecuentes.
    // Los valores alejados de la media son cada vez menos frecuentes.
    // Los valores extremadamente alejados son muy raros.
    // _rng → genera bits/números pseudoaleatorios
    //noise → transforma esa aleatoriedad para que siga una distribución normal

    for (const auto& id : ids) {
        // overrideIterator
        // primero verificamos si hay un valor forzado para la instancia
        auto overrideIt = _cpuOverrides.find(id);
        // Si encontró valor forzado, (no terminó en end) ->  
        double value = (overrideIt != _cpuOverrides.end())
                            ? overrideIt->second // como overrideIt apunta a objeto, y nuestros objetos son (id, value), obtenemos segundo elemento
                            : clamp(_baselineCpuPercent + noise(_rng), 0.0, 100.0); // para que valor no salga de rango (0,100) -> si es menor -> 0, si es mayor -> 100
        result[id] = value;
    }
    return {FetchStatus::Ok, result};
}

FetchResult<IMetricsSource::MetricSeriesByInstance> FakeMetricsSource::GetCpuHistory(
    const vector<string>& ids, chrono::seconds window) {

    if (ConsumeFailureFlag()) {
        return {FetchStatus::ApiError, {}};
    }

    MetricSeriesByInstance result;
    auto now = chrono::system_clock::now();
    normal_distribution<double> noise(0.0, 5.0);

    // Para cada id, 
    for (const auto& id : ids) {

        MetricSeries series; //vector de metric samples
        // si hay valor de ese id en cpuOve.., lo tomamos como baseline, si no, usamos baseline de objeto
        double baseline = _cpuOverrides.count(id) ? _cpuOverrides.at(id) : _baselineCpuPercent;

        // avanzamos de a 60 seg, hasta completar ventana (obteniendo los n registros -> 1 dato para cada minuto)
        // es el tiempo transcurrido desde el comienzo de la ventana 
        for (auto elapsed = chrono::seconds(0); elapsed < window; elapsed += SAMPLE_STEP) {
            // añadimos metricSample (timestamp, value)
            // ejm: 12:00 - (10 - 0) -> 11:50
            // 12:00 - (10 - 2) -> 11:52 -> a medida que avanza ventana, avanza tiempo
            series.push_back({now - (window - elapsed), clamp(baseline + noise(_rng), 0.0, 100.0)});
        }
        // guardamos el vector series, asociado a id de instancia
        result[id] = move(series);
    }
    return {FetchStatus::Ok, result};
}

FetchResult<double> FakeMetricsSource::GetCurrentRequest() {
    if (ConsumeFailureFlag()) {
        return {FetchStatus::ApiError, 0.0};
    }
    // normal distribution con media 0 y desviación estandar 1/10 de baselinerequest -> 
    // nosotros seria 100 * 0.1 = 10 <- desviación
    normal_distribution<double> noise(0.0, _baselineRequests * 0.1);

    //obtenemos máximo entre 0 y baseline con ruido (porque puede dar num negativo, y request no pueden serlo)
    // y no podemos clamp porque no hay máximo límite.
    return {FetchStatus::Ok, max(0.0, _baselineRequests + noise(_rng))};
}

FetchResult<IMetricsSource::MetricSeries> FakeMetricsSource::GetRequestHistory(chrono::seconds window) {
    if (ConsumeFailureFlag()) {
        return {FetchStatus::ApiError, {}};
    }

    //misma lógica de getCpuHistory, solo que con max 
    MetricSeries series;
    auto now = chrono::system_clock::now();
    normal_distribution<double> noise(0.0, _baselineRequests * 0.1);

    // creamos dato por minuto
    for (auto elapsed = chrono::seconds(0); elapsed < window; elapsed += SAMPLE_STEP) {
        series.push_back({now - (window - elapsed), max(0.0, _baselineRequests + noise(_rng))});
    }
    return {FetchStatus::Ok, series};
}

void FakeMetricsSource::SetCpuOverride(const string& instanceId, double value) {
    _cpuOverrides[instanceId] = value;
}

void FakeMetricsSource::SetNextCallFails(bool shouldFail) {
    _forceNextCallToFail = shouldFail;
}