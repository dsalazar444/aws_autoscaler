#include "LinearRegressionPredictor.h"
#include <algorithm>

using namespace std;

LinearRegressionPredictor::LinearRegressionPredictor(size_t minSamples)
    : _minSamples(minSamples) {}  

optional<double> LinearRegressionPredictor::EstimateAt(
    const MetricSeries& knownSamples,
    std::chrono::system_clock::time_point targetTimestamp) {

    // NOTA: X -> tiempo, y -> CPU
    // --- Paso 1: ¿hay suficientes puntos para siquiera intentarlo? ---
    if (knownSamples.size() < _minSamples) {
        return std::nullopt; // nullopt es que no se puede predecir
    }

    // --- Paso 2: pasar los timestamps a numeros pequeños y manejables ---
    // Un time_point convertido directo a "segundos desde epoch" da numeros
    // gigantes (~1.7 mil millones). Usarlos tal cual en la regresion mete
    // sumas de cuadrados enormes y pierde precision en punto flotante.
    // Por eso medimos todo relativo al punto MAS VIEJO de la serie: ese pasa
    // a ser x=0, y los demas son "segundos desde el mas viejo" -- numeros
    // pequeños, mucho mas estables.
    //
    // OJO: MetricSeries deberia venir siempre ordenada ascendente (ver la
    // invariante documentada junto a su definicion), pero no confiamos
    // ciegamente en eso aqui -- buscamos el minimo real en vez de asumir que
    // es knownSamples.front(). Es barato y evita un bug silencioso si algun
    // dia algo construye la serie sin respetar el orden.
    auto referenceTime = min_element(knownSamples.begin(), knownSamples.end(),
                                           [](const MetricSample& a, const MetricSample& b) {
                                               return a.timestamp < b.timestamp;
                                           })->timestamp;

    // Lo que hace min_element es iterar sobre un rango, tomando de a dos elementos, y compara menor -> segun bool, cambia el "min"
    // al que apunta, y compara min con el siguiente. Y así hasta obtener menor.
    // Apunta a ese, entocnes min devuelve iterador, con ->timestamp obtenemos dato.

    // en nuestro codigo, metricsample tiene timestamp de tipo time_point -> un time_point son los segundos desde 1970
    // hasta timestamp -> muy grande -> 
    // Usamos estrategia diferente -> escogemos el más antiguo, y calculamos segundos desde él -> es como settear nuestro
    // propio "1970"
    // lambda que recibe un time_point t, y a ese t le restamos nuestro tiempo más viejo -> resta da un "duration" type
    // lo convertimos a duration<double>, y con count extraemos número
    // [&] porque usamos var que está fuera -> con eso capturamos variables externas por referencia
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

    // acumulamos valores de x, y valores de y
    for (const auto& sample : knownSamples) {
        sumX += toRelativeSeconds(sample.timestamp);
        sumY += sample.value;
    }

    // calculamos promedios del total de cada eje -> obtenemos cpu promedio y timestamp promedio
    double meanX = sumX / static_cast<double>(n);
    double meanY = sumY / static_cast<double>(n);

    double numerator = 0.0;    // Σ((x_i - meanX)(y_i - meanY))
    double denominator = 0.0;  // Σ((x_i - meanX)^2)
    // slope = numerator / denominator

    for (const auto& sample : knownSamples) {
        double dx = toRelativeSeconds(sample.timestamp) - meanX; // ¿Qué tan lejos está este x del promedio de X?
        double dy = sample.value - meanY; // ¿Qué tan lejos está este y del promedio de Y?
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

    // Al final, num / den es pendiente -> indica cuanto aumenta cpu por segundo
    double slope = numerator / denominator;
    double intercept = meanY - slope * meanX;

    // --- Paso 5: evaluar la recta en el instante pedido ---
    // targetTimestamp se convierte a la MISMA escala relativa que usamos para
    // ajustar la recta (segundos desde el primer punto de knownSamples).
    double targetX = toRelativeSeconds(targetTimestamp);
    return intercept + slope * targetX; // y -> cpu
}