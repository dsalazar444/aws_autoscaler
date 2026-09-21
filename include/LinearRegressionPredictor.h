#pragma once

#include "IValuePredictor.h"

// Implementacion concreta de IValuePredictor via regresion lineal simple
// (minimos cuadrados) sobre los puntos conocidos. Es el motor matematico
// del modulo Proactivo -- le da un estimado a quien se lo pida: Validator
// (para rellenar huecos) o, mas adelante, el analizador que compara la
// prediccion contra los umbrales de scale-out/in.
class LinearRegressionPredictor : public IValuePredictor {
public:
    // minSamples: minimo de puntos necesarios para intentar un ajuste. Con muy
    // pocos puntos una recta "ajusta perfecto" pero no significa nada -> es
    // ruido, no tendencia real. Por defecto pedimos al menos 3. 
    explicit LinearRegressionPredictor(size_t minSamples = 3);

    std::optional<double> EstimateAt(
        const MetricSeries& knownSamples, 
        std::chrono::system_clock::time_point targetTimestamp) override;

private:
    size_t _minSamples;
};