#include "Analytics.h"

#include <algorithm>
#include <cmath>

using namespace std;

namespace Analytics {

double CalculatePercentile(vector<double> values, double percentile) {
    // ordenamos una COPIA -- no queremos modificar la lista del caller
    sort(values.begin(), values.end());

    size_t n = values.size();
    if (n == 0) {
        // no deberia llamarse con una lista vacia -- CalculateGlobalCpuSeries
        // ya se encarga de no invocar esto sin al menos un valor en el bucket
        return 0.0;
    }

    // metodo del rango mas cercano (nearest-rank): el "puesto" que interesa
    // es ceil(percentile/100 * n), 1-indexado (no es un vector, empieza en 1), acotado a [1, n]
    // Obtenemos procentaje del percentil con perc/ 100, y multiplicamos por tamaño para 
    // obtener posición
    size_t rank = static_cast<size_t>(ceil((percentile / 100.0) * static_cast<double>(n)));
    // forma estándar de calcular qué valor en un conjunto de datos corresponde a un percentil específico
    // redondeamos hacia arriba con ceil

    rank = clamp<size_t>(rank, 1, n); // garantiza que valor si este entre 1 y n, <1 se convierte a minimo, y >n se convierte a n

    return values[rank - 1];  // -1 porque el vector es 0-indexado
}

double CalculateP95(const vector<double>& values) {
    return CalculatePercentile(values, 95.0);
}

// recibe n buckets, y va uno a uno. P95 se alica a n datos de mismo bucket (timestamp
// solo metricseries (~byinstance) porque es un valor por timestamp  del sistema
MetricSeries CalculateGlobalCpuSeries(
    const MetricSeriesByInstance& perInstanceHistory,
    const vector<chrono::system_clock::time_point>& expectedTimestamps,
    chrono::seconds timestampTolerance) {

    MetricSeries globalSeries;

    for (const auto& bucketTimestamp : expectedTimestamps) {

        // --- recolectar el valor de CADA instancia en este bucket ---
        vector<double> valuesAtBucket;

        for (const auto& [id, series] : perInstanceHistory) {
            // series es un vector de MetricSample
            auto it = find_if(series.begin(), series.end(), [&](const MetricSample& sample) {
                auto diff = sample.timestamp > bucketTimestamp ? sample.timestamp - bucketTimestamp
                                                                 : bucketTimestamp - sample.timestamp;
                return diff <= timestampTolerance;
                // calculamos para cada dato su diferencia con el timestamp buscado, si es menor a la 
                // tolerancia, salimos de ciclo.
            });
            if (it != series.end()) {
                valuesAtBucket.push_back(it->value);
            }
            // si esta instancia no tiene dato en este bucket (ni siquiera
            // despues de Validator), simplemente no participa en el p95 de
            // este bucket -- Analytics no rellena nada, solo reduce
        }

        if (valuesAtBucket.empty()) {
            // ninguna instancia aporto dato para este bucket -- se omite de
            // la serie global, no se inventa un 0
            continue;
        }
        // ya que obtuvimos los valores de ese timestamp, calculamos sus p95
        globalSeries.push_back({bucketTimestamp, CalculateP95(valuesAtBucket)});
    }

    return globalSeries;
}

}  // namespace Analytics