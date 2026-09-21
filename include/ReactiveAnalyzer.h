#pragma once

#include "IMetricsSource.h"

// Que "opina" el modulo Reactivo sobre el estado ACTUAL (no predicho) de la
// CPU global. Es una SEÑAL para que Controller la combine con Proactivo --
// igual que ProactiveAnalyzer, nunca decide ni ejecuta nada.
enum class ReactiveSignal {
    SustainedHigh,     // CPU > umbral alto durante TODA la ventana alta (n min)
    SustainedLow,      // CPU < umbral bajo durante TODA la ventana baja (2n min)
    Normal,            // ninguna de las dos condiciones sostenidas se cumple
    InsufficientData   // no hay suficiente historico para evaluar "sostenido"
                       // con confianza (ej. el sistema acaba de arrancar)
};

class ReactiveAnalyzer {
public:
    // highThreshold/lowThreshold: los mismos umbrales de siempre (70/30).
    // highWindow: cuanto tiempo debe sostenerse por encima de highThreshold
    //   para disparar scale-out ("n min").
    // lowWindow: cuanto tiempo debe sostenerse por debajo de lowThreshold
    //   para considerar scale-in ("2n min" -- mas largo a proposito, ya
    //   que reducir es la accion mas riesgosa).
    ReactiveAnalyzer(double highThreshold, double lowThreshold,
                      std::chrono::seconds highWindow, std::chrono::seconds lowWindow);

    // globalCpuHistory: la serie YA REDUCIDA que produce Analytics (p95 entre
    // instancias, un valor por bucket de tiempo) -- igual que ProactiveAnalyzer,
    // Reactivo no sabe nada de instancias individuales.
    ReactiveSignal Evaluate(const MetricSeries& globalCpuHistory,
                             std::chrono::system_clock::time_point now);

private:
    double _highThreshold;
    double _lowThreshold;
    std::chrono::seconds _highWindow;
    std::chrono::seconds _lowWindow;
};