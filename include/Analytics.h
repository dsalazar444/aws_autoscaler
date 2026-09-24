#pragma once

#include "IMetricsSource.h"

#include <vector>

// Analytics reduce el historico YA VALIDADO (por instancia) a UNA sola serie
// global: el p95 entre instancias en cada bucket de tiempo. Es lo que reciben
// Reactivo y Proactivo -- ninguno de los dos sabe que existen varias
// instancias, solo ven esta serie ya reducida, que representa el estado del sistema.
//
// No tiene estado ni depende de Metricas/Validator/Predictor -- es puro
// calculo sobre los datos que le pasan, por eso son funciones sueltas en vez
// de una clase con constructor. Además de que funcionará como modulo de utils, en el cual
// estarán las funciones que no son propias de una clase, pero son usadas por ellas.
//
// NOTA: esto solo hace falta para CPU. Requests ya llega como un solo
// agregado del target group (RequestCountPerTarget promedio) -- no hay nada
// que reducir entre instancias ahi, esa serie se usa tal cual.
namespace Analytics {

// Percentil generico (metodo del rango mas cercano / nearest-rank).
// percentile va de 0 a 100. Recibe una copia de `values`, no modifica la
// del caller.
double CalculatePercentile(std::vector<double> values, double percentile);

// Atajo para el caso que usamos en todo el proyecto.
double CalculateP95(const std::vector<double>& values);

// Reduce el historico por instancia a una sola serie global de p95, un punto
// por cada timestamp en `expectedTimestamps` -- el MISMO grid que uso
// Validator para rellenar huecos. Reutilizar ese grid aqui es lo que permite
// agrupar consistentemente los puntos de distintas instancias por bucket.
//
// Si NINGUNA instancia tiene dato en un bucket dado (ni siquiera despues de
// Validator), ese bucket se omite de la serie global -- Analytics no rellena
// nada, solo reduce lo que ya llego validado.
MetricSeries CalculateGlobalCpuSeries(
    const MetricSeriesByInstance& perInstanceHistory,
    const std::vector<std::chrono::system_clock::time_point>& expectedTimestamps,
    std::chrono::seconds timestampTolerance = std::chrono::seconds(15));

}  // namespace Analytics