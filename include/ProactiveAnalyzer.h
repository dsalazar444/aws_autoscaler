#pragma once

#include "IMetricsSource.h"
#include "IValuePredictor.h"

#include <memory>

// Que "opina" el modulo Proactivo sobre el futuro cercano de la CPU global.
// Es una SEÑAL para que Controller la combine con lo que diga Reactivo --
// ProactiveAnalyzer nunca decide MAINTAIN/INCREASE/REDUCE, y nunca ejecuta
// nada. Solo informa.
enum class ProactiveSignal {
    // se espera superar el umbral alto (ej. 70%) dentro del horizonte --
    // dispara la condicion OR de scale-out, y VETA cualquier scale-in
    PredictsAboveHighThreshold,

    // se espera quedar por debajo del umbral alto -- no hay riesgo inminente
    // segun la tendencia; no veta el scale-in
    PredictsSafe,

    // no hay suficiente historico para predecir con confianza (EstimateAt
    // devolvio nullopt). Segun lo que ya acordaron: para scale-in esto se
    // trata como veto por defecto (postura conservadora, ver Controller);
    // para scale-out simplemente el Reactivo queda decidiendo solo este ciclo
    Unknown
};

class ProactiveAnalyzer {
public:
    // predictor: el motor matematico (ej. LinearRegressionPredictor). 
        // -> shared_ptr porque es un objeto que tendra varios punteros, y ellos deben ser
        // propietarios de este -> uno no puede eliminarlo, porque queda problema de dangling pointer
        // (los otros ptrs apuntan a nada) -> con shared, permite varios propietarios del mismo objeto, y gestiona
        // cuando destruirlo -> cuando no hayan propietarios, mientras haya alguno, se mantiene
    // horizon: cuanto al futuro mirar -- tiempo de creacion de VM + margen.
    // highThreshold: el mismo umbral de scale-out que usa Reactivo (ej. 70.0).
    ProactiveAnalyzer(std::shared_ptr<IValuePredictor> predictor,
                       std::chrono::seconds horizon,
                       double highThreshold);

    // globalCpuHistory: la serie YA REDUCIDA (del sistema, no de una VM) que produce
    //  Analytics (p95 entre instancias, un valor por bucket de tiempo) -- ProactiveAnalyzer
    // no sabe nada de instancias individuales, solo trabaja sobre la serie global.
    // now: el instante desde el cual se cuenta el horizonte (parametro, no
    // system_clock::now() interno, para poder probarlo con un tiempo fijo).
    ProactiveSignal Evaluate(const MetricSeries& globalCpuHistory,
                              std::chrono::system_clock::time_point now);

private:
    std::shared_ptr<IValuePredictor> _predictor;
    std::chrono::seconds _horizon;
    double _highThreshold;
};