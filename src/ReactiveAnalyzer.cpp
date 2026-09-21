#include "ReactiveAnalyzer.h"

#include <algorithm>

using namespace std;

namespace {

// Resultado interno de revisar UNA condicion sostenida (alta o baja) --
// no se expone fuera de este archivo, ReactiveSignal es lo publico.
enum class SustainedCheckResult { Sustained, NotSustained, InsufficientData };

// Tolerancia pequeña al medir si el historico "cubre" el inicio de la
// ventana -- para no ser demasiado estrictos por un desfase de pocos
// segundos entre buckets de muestreo.
constexpr chrono::seconds COVERAGE_TOLERANCE{30};

// Revisa si TODOS los puntos dentro de [now - window, now] cumplen la
// condicion (por encima o por debajo de threshold, segun checkAbove).
//
// OJO: es "todos los puntos", no "el promedio". Un pico de 95% rodeado de
// valores en 40% podria promediar por encima de 70%, pero eso NO es lo mismo
// que CPU sostenido alto -- exigir que cada punto individual cumpla es mas
// fiel a lo que "sostenido" deberia significar.
// Data se saca de info de objeto
SustainedCheckResult CheckSustained(const MetricSeries& history,
                                    chrono::system_clock::time_point now,
                                    chrono::seconds window,
                                    double threshold,
                                    bool checkAbove) {
    // windowStart es el timestamp en el que empieza la ventana (ejm: window = 5 min, y now = 10:15, windowStart = 10:10)
    auto windowStart = now - window;

    // --- Paso 1: quedarnos solo con los puntos dentro de la ventana, porque history puede
    // tener más ---
    std::vector<const MetricSample*> pointsInWindow;
    for (const auto& sample : history) {
        if (sample.timestamp >= windowStart && sample.timestamp <= now) {
            pointsInWindow.push_back(&sample);
        }
    }

    if (pointsInWindow.empty()) {
        // no hay NINGUN dato en toda la ventana -- no podemos opinar
        return SustainedCheckResult::InsufficientData;
    }

    // --- Paso 2: ¿el historico realmente cubre TODA la ventana? ---
    // Si el punto mas viejo que cae dentro del rango es bastante mas nuevo
    // que windowStart (permitimos ligera discrepancia), en realidad no tenemos dato para el inicio de la
    // ventana -- eso es "no lo sabemos", no "no esta sostenido". Ejemplo
    // tipico: el sistema acaba de arrancar y todavia no hay n minutos de
    // historico acumulado.
    auto earliestInWindow = std::min_element(
        pointsInWindow.begin(), pointsInWindow.end(),
        [](const MetricSample* a, const MetricSample* b) { return a->timestamp < b->timestamp; });

    if ((*earliestInWindow)->timestamp > windowStart + COVERAGE_TOLERANCE) {
        return SustainedCheckResult::InsufficientData;
    }

    // --- Paso 3: revisar que CADA punto cumpla la condicion ---
    for (const auto* sample : pointsInWindow) {
        // checkAbove indica si me piden mirar si se mantiene por encima, si no, es porque piden 
        // mirar si se manriene por abajo
        bool meetsCondition = checkAbove ? (sample->value > threshold) : (sample->value < threshold);
        if (!meetsCondition) {
            // un solo punto que rompe la condicion ya invalida "sostenido"
            return SustainedCheckResult::NotSustained;
        }
    }
    return SustainedCheckResult::Sustained;
}

}  // namespace

ReactiveAnalyzer::ReactiveAnalyzer(double highThreshold, double lowThreshold,
                                    std::chrono::seconds highWindow,
                                    std::chrono::seconds lowWindow)
    : _highThreshold(highThreshold),
      _lowThreshold(lowThreshold),
      _highWindow(highWindow),
      _lowWindow(lowWindow) {}

ReactiveSignal ReactiveAnalyzer::Evaluate(const MetricSeries& globalCpuHistory,
                                           std::chrono::system_clock::time_point now) {

    // Revisamos primero la condicion ALTA: ventana mas corta (n min), asi
    // que es la que mas rapido alcanza a tener suficiente historico, y ademas
    // es la mas urgente de las dos (evitar quedarse sin capacidad pesa mas
    // que perder una oportunidad de ahorrar).
    // Indicamos checkAbove como true, y si se mantiene, retornamos señal de sustainedHigh (nuestra opinión)
    auto highResult = CheckSustained(globalCpuHistory, now, _highWindow, _highThreshold,
                                      /*checkAbove=*/true);
    if (highResult == SustainedCheckResult::Sustained) {
        return ReactiveSignal::SustainedHigh;
    }

    // Solo si NO esta sostenido alto (no por falta de datos) tiene sentido
    // preguntar por la condicion BAJA -- si ya sabemos que hay riesgo de
    // sobrecarga, no hace falta ni evaluar si es seguro reducir.
    auto lowResult = CheckSustained(globalCpuHistory, now, _lowWindow, _lowThreshold,
                                     /*checkAbove=*/false);
    if (lowResult == SustainedCheckResult::Sustained) {
        return ReactiveSignal::SustainedLow;
    }

    // Si CUALQUIERA de las dos revisiones no tuvo suficiente historico, todo
    // el resultado de este ciclo es incierto -- no queremos decir "Normal"
    // (que implica "revisamos y esta todo bien") cuando en realidad no
    // pudimos revisar con confianza.
    if (highResult == SustainedCheckResult::InsufficientData ||
        lowResult == SustainedCheckResult::InsufficientData) {
        return ReactiveSignal::InsufficientData;
    }

    return ReactiveSignal::Normal;
}