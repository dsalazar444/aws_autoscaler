#pragma once

#include "IMetricsSource.h"
#include <optional>

// Contrato MINIMO que el Predictor real debera cumplir.
//
// Devuelve std::optional<double> en vez de double a secas: nullopt significa
// "no tengo suficiente informacion, o los datos no permiten un estimado
// confiable" (ej. muy pocos puntos, o todos los puntos en el mismo instante).
// Esto es necesario para que quien lo use (Validator, o mas adelante el
// modulo Proactivo/Controller) pueda distinguir "prediccion real" de "no
// puedo predecir" sin depender de un valor magico ambiguo.
class IValuePredictor {
public: 
    virtual ~IValuePredictor() = default;

    // función que permite estimar, a partir de unas samples, la metrica en cierto momento futuro
    virtual std::optional<double> EstimateAt(
        const MetricSeries& knownSamples, 
        std::chrono::system_clock::time_point targetTimestamp) = 0;
};