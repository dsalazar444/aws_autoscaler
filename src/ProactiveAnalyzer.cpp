#include "ProactiveAnalyzer.h"

using namespace std;
ProactiveAnalyzer::ProactiveAnalyzer(shared_ptr<IValuePredictor> predictor,
                                      chrono::seconds horizon,
                                      double highThreshold)
    : _predictor(move(predictor)), _horizon(horizon), _highThreshold(highThreshold) {}

ProactiveSignal ProactiveAnalyzer::Evaluate(const MetricSeries& globalCpuHistory,
                                            chrono::system_clock::time_point now) {
    // --- Paso 1: pedirle al motor matematico un estimado en "ahora + horizonte" ---
    auto targetTimestamp = now + _horizon;
    auto estimated = _predictor->EstimateAt(globalCpuHistory, targetTimestamp);

    // --- Paso 2: sin suficiente historico, no hay opinion que dar ---
    if (!estimated.has_value()) {
        return ProactiveSignal::Unknown;
    }

    // --- Paso 3: interpretar el numero contra el umbral alto ---
    // No comparamos contra el umbral bajo (30%) aqui -- esa parte de la regla
    // de scale-in ya la cubre Reactivo con el CPU actual sostenido. Proactivo
    // solo necesita responder: "¿se predice peligro (superar el umbral alto)
    // o no?"
    if (*estimated > _highThreshold) {
        return ProactiveSignal::PredictsAboveHighThreshold;
    }
    return ProactiveSignal::PredictsSafe; //por sí sola, no es suficiente para autorizar un
    // scale-in — sería una condición necesaria pero no suficiente, y Controller haría la verificación fina con N-1 antes de confirmar.
}