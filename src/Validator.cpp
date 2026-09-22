#include "Validator.h"

#include <algorithm>

using namespace std;

namespace {

// Tolerancia para considerar que un timestamp REAL "coincide" con uno
// ESPERADO del grid -- por pequeños desfases de publicacion de CloudWatch
// (el mismo tipo de retraso que ya vimos en GetCurrentCpu).
constexpr chrono::seconds TIMESTAMP_TOLERANCE{15};

// Genera la lista de instantes "esperados" dentro de la ventana, espaciados
// por `period` (ejm: 60s) -- el grid que CloudWatch deberia haber llenado si no hubiera
// huecos. Sirve como referencia para saber QUE puntos faltan, no para
// inventar datos por si solo.
vector<chrono::system_clock::time_point> BuildExpectedTimestamps(
    chrono::system_clock::time_point now, // finish time
    chrono::seconds window,
    chrono::seconds period) {

    vector<chrono::system_clock::time_point> expected;

    for (auto t = now - window; t <= now; t += period) { // 15:00 - 5 min = 14:55 -> 14:55 <= 15:00, t= 14:55 + 1:00 = 14:56
        // inicio desde ventana (inicio) -> voy avanzando de minuto en minuto, hasta llegar a now (tiempo "final")
        expected.push_back(t);

    }
    return expected;
}

// ¿la serie ya tiene un punto real cerca de `target` (dentro de la
// tolerancia)? Si es asi, no hay hueco que rellenar en ese instante.
bool HasSampleNear(const MetricSeries& series, chrono::system_clock::time_point target) {
    for (const auto& sample : series) {
        // para cada MetricSample, miramos si es mayor o menor, y operamos segun caso para obtener distancia entre
        // cada sample, y target
        auto diff = sample.timestamp > target ? sample.timestamp - target : target - sample.timestamp;
        // Si la diferencia/distancia es menor a tolerancia, retornamos true
        if (diff <= TIMESTAMP_TOLERANCE) {
            return true;
        }
    }
    return false;
}

}  // namespace

Validator::Validator(shared_ptr<IValuePredictor> predictor, double completenessThreshold)
    : _predictor(move(predictor)), _completenessThreshold(completenessThreshold) {}

ValidationResult<unordered_map<string, double>> Validator::ValidateCurrentCpus(
    const vector<string>& ids,
    const unordered_map<string, double>& rawCurrentCpu,
    const MetricSeriesByInstance& cpuHistory,
    chrono::system_clock::time_point targetTimestamp) {

    // --- Paso 1: medir que tan completo llego el dato ---
    // `ids` es la lista de instancias que ESPERABAMOS ver. `rawCurrentCpu` es lo
    // que Metricas realmente pudo traer -- cada id de `ids` que NO aparece como
    // llave en rawCurrentCpu es una instancia sin dato este ciclo.
    size_t expectedCount = ids.size();
    size_t presentCount = rawCurrentCpu.size();

    if (expectedCount == 0) {
        // no hay instancias que revisar (ej. el ASG esta en 0) -- esto no es
        // un problema de calidad de dato, es un mapa vacio legitimo
        return {ValidationStatus::Complete, {}};
    }

    // --- Paso 2: si llego completo, no hay nada que hacer ---
    if (presentCount == expectedCount) {
        return {ValidationStatus::Complete, rawCurrentCpu};
    }

    double completeness = static_cast<double>(presentCount) / static_cast<double>(expectedCount); // 5 de 8 llegaron -> 5 es present, 8 es expected

    // --- Paso 3: si falta demasiado, ni lo intentamos rellenar ---
    // Rellenar 4 de 5 instancias faltantes con puras adivinanzas del Predictor
    // ya no es "un dato con huecos", es basicamente inventarse el ciclo entero.
    // Mejor decirle claramente a quien nos llama "no confies en esto".
    if (completeness < _completenessThreshold) {
        return {ValidationStatus::InsufficientData, {}};
    }

 
    // --- Paso 4: faltan ALGUNAS instancias (menos que el umbral) -> rellenar ---
    // Partimos de una copia de lo que si llego, y por ende tiene value, y solo tocamos las que faltan.
    unordered_map<string, double> filled = rawCurrentCpu;

    for (const auto& id : ids) {
        if (filled.count(id)) { // count busca idm si existe retorna 1, si no, 0
            continue;  // esta instancia ya tenia dato real, no la tocamos
        }

        auto historyIt = cpuHistory.find(id); // SI no está en filled, obtenemos SU historial
        if (historyIt == cpuHistory.end() || historyIt->second.empty()) {
            // no tenemos NADA de historico de esta instancia (ej. acaba de
            // nacer) -- no hay con que estimar, mejor dejarla fuera del
            // resultado que inventar un numero sin ninguna base real -> además de que si acaba de nacer, es muy probable que su
            // cpu sea muy baja -> poco riesgo

            // no va return insufficientData porque es que falta datos historicos de una instancia, no de todos/mayoria los
            // elementos enviados -> insufficient se basa en porcentaje de datos faltantes, y si llegó hasta acá, es porque la 
            // mayoria de datos están
            continue;
        }

        // le pedimos al Predictor un estimado en targetTimestamo, usando el propio pasado de ESTA
        // instancia -- nunca el historico de otra, cada maquina tiene su carga
        auto estimated = _predictor->EstimateAt(historyIt->second, targetTimestamp); // retorna double
        if (!estimated.has_value()) { // has_value es funcion de tipos que son optional, y lo que retorna el predictor lo es. 
            // el predictor tampoco pudo estimar con lo poco que hay (ej. muy
            // pocos puntos en el historico de esta instancia) -- la dejamos
            // fuera, igual que si no hubiera historico en absoluto
            continue;
        }
        filled[id] = *estimated; // obtenemos valor contenido en optional -> optional NO RETORNA PUNTERO, solo que esa clase, sobreescribe '*' para que permita acceder a sus value
    }

    return {ValidationStatus::Filled, filled};
}

ValidationResult<MetricSeriesByInstance> Validator::ValidateCpuHistory(
    const vector<string>& ids,
    const MetricSeriesByInstance& rawCpuHistory,
    chrono::system_clock::time_point now,
    chrono::seconds window,
    chrono::seconds period) {

    // --- Paso 1: el grid de instantes que "deberiamos" tener por instancia ---
    auto expectedTimestamps = BuildExpectedTimestamps(now, window, period);

    MetricSeriesByInstance validated;
    bool anyGapFilled = false;

    // --- Paso 2: procesar cada instancia por separado ---
    for (const auto& id : ids) {
        auto it = rawCpuHistory.find(id);

        // si no tenemos NI UN punto de esta instancia, no hay con que
        // estimar nada -- se excluye de este ciclo (no de forma permanente,
        // el proximo ciclo se vuelve a intentar con datos frescos)
        if (it == rawCpuHistory.end() || it->second.empty()) {
            continue;
        }

        const MetricSeries& rawSeries = it->second; // obtenemos su value, que ese metricSeries
        // con & creamos referencia, que es que guarda misma dirección que it->second
        MetricSeries filledSeries = rawSeries;  // arrancamos con lo que si es real
        // con esto, creamos copia de historial de la instancia especifica para poder modificar

        // --- Paso 3: revisar bucket por bucket (metricsample por metricsample) si falta algo ---
        for (const auto& expectedTs : expectedTimestamps) { // vamos por cada uno de los metricSample/timestamp, que deberia tener bucket
            if (HasSampleNear(rawSeries, expectedTs)) {
                continue;  // ya hay dato real cerca de este instante, no tocar
            }

            // le pedimos al Predictor un estimado usando el propio pasado de
            // ESTA instancia (que esta en rawSeries)
            auto estimated = _predictor->EstimateAt(rawSeries, expectedTs);
            if (estimated.has_value()) {
                filledSeries.push_back({expectedTs, *estimated});
                anyGapFilled = true;
            }
            // si el predictor tampoco pudo (ej. muy pocos puntos reales en
            // rawSeries), ese bucket(metricSample) simplemente queda sin dato (ese timestamp no estará en el metricSerie 
            // de id que retornemos -> no es que se añade vacio, es que no se añade)
            // -- Analytics debe poder calcular el p95 de un bucket aunque no todas las instancias tengan valor ahi
        }

        // insertamos los rellenos al final del bucle, asi que hay que
        // reordenar por timestamp antes de entregar la serie
        sort(filledSeries.begin(), filledSeries.end(),
                  [](const MetricSample& a, const MetricSample& b) {
                      return a.timestamp < b.timestamp;
                  });

        validated[id] = move(filledSeries); // filledseries es solo historial de una cpu, por eso lo ponemos en su id
    }

    // --- Paso 4: completitud GLOBAL -- cuantas instancias de `ids` terminaron
    // con AL MENOS una serie utilizable (aunque le hayan faltado algunos
    // buckets puntuales) ---
    double completeness = ids.empty()
                               ? 1.0 // para evitar dividir entre 0 -> 0 de 0 instancias estan completas -> no hay incumplimiento de cumplitud
                            // entonces no entra a insufficientdata, pero con anygapfilled = false, daría complete
                               : static_cast<double>(validated.size()) / static_cast<double>(ids.size());
                               // verificamos si ids está vacio, y si no, obtenemos cuán completas quedaron -> ids validadas / ids 
                               // que llegaron 
                               // Recordar que validated sale de rawCPU (es una copia), entonces lo que verificamos es la relacion
                               // de esos con los ids "esperados"/totales -> por ejem: son 10 ids, pero en raw nos mandan solo 2
                               // -> debemos indicar que esa fuente que predijimos "con solo dos ids" es insuficiente, pues, 
                               // independientemente de si fue proque mandaron pocos ids, o porque tiene muchos huecos, 
                               // no lo pudimos corregir (predecir y llenarlo) para el módulo que lo pidió

    if (completeness < _completenessThreshold) {
        // en los primeros ciclos del sistema, esto va a pasar seguido: casi
        // no hay historico todavia, asi que casi ninguna instancia cumple.
        // Es exactamente la situacion que hace que Controller deba mantener
        // capacidad mientras se acumula suficiente historia
        return {ValidationStatus::InsufficientData, {}};
    }

    return {anyGapFilled ? ValidationStatus::Filled : ValidationStatus::Complete, validated};
}