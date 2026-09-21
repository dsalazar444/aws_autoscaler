#include "LinearRegressionPredictor.h"

using namespace std;

LinearRegressionPredictor::LinearRegressionPredictor(size_t minSamples)
    : _minSamples(minSamples) {}  

optional<double> LinearRegressionPredictor::EstimateAt(
    const MetricSeries& knownSamples,
    std::chrono::system_clock::time_point targetTimestamp) {

    // --- Paso 1: ¿hay suficientes puntos para siquiera intentarlo? ---
    if (knownSamples.size() < _minSamples) {
        return std::nullopt; // nullopt es que no se puede predecir
    }

    // --- Paso 2: pasar los timestamps a numeros pequeños y manejables ---
    // Un time_point convertido directo a "segundos desde epoch" da numeros
    // gigantes (~1.7 mil millones). Usarlos tal cual en la regresion mete
    // sumas de cuadrados enormes y pierde precision en punto flotante.
    // Por eso medimos todo relativo al PRIMER punto de la serie: ese pasa
    // a ser x=0, y los demas son "segundos desde el primero" -- numeros
    // pequeños, mucho mas estables.
    auto referenceTime = knownSamples.front().timestamp;
    auto toRelativeSeconds = [&](std::chrono::system_clock::time_point t) {
        return std::chrono::duration<double>(t - referenceTime).count();
    };

    // --- Paso 3: minimos cuadrados (regresion lineal simple) ---
    // Ajustamos una recta y = intercept + slope * x, con las formulas
    // clasicas de minimos cuadrados:
    //   slope     = Σ((x_i - meanX)(y_i - meanY)) / Σ((x_i - meanX)^2)
    //   intercept = meanY - slope * meanX
    double sumX = 0.0;
    double sumY = 0.0;
    size_t n = knownSamples.size();

    for (const auto& sample : knownSamples) {
        sumX += toRelativeSeconds(sample.timestamp);
        sumY += sample.value;
    }
    double meanX = sumX / static_cast<double>(n);
    double meanY = sumY / static_cast<double>(n);

    double numerator = 0.0;    // Σ((x_i - meanX)(y_i - meanY))
    double denominator = 0.0;  // Σ((x_i - meanX)^2)

    for (const auto& sample : knownSamples) {
        double dx = toRelativeSeconds(sample.timestamp) - meanX;
        double dy = sample.value - meanY;
        numerator += dx * dy;
        denominator += dx * dx;
    }

    // --- Paso 4: caso degenerado ---
    // Si todos los timestamps son practicamente iguales, denominator ~ 0 y no
    // hay recta que ajustar. No es un error de calculo -- simplemente no hay
    // informacion de tendencia en el tiempo dentro de estos puntos.
    if (denominator < 1e-9) {
        return std::nullopt;
    }

    double slope = numerator / denominator;
    double intercept = meanY - slope * meanX;

    // --- Paso 5: evaluar la recta en el instante pedido ---
    // targetTimestamp se convierte a la MISMA escala relativa que usamos para
    // ajustar la recta (segundos desde el primer punto de knownSamples).
    double targetX = toRelativeSeconds(targetTimestamp);
    return intercept + slope * targetX;
}