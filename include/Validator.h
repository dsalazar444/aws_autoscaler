#pragma once

#include "IMetricsSource.h"
#include "IValuePredictor.h"

#include <memory>

// Que tan "confiable" quedo un dato despues de revisarlo.
//
// - Complete: llegaron TODOS los datos que esperabamos, no hubo que tocar nada.
// - Filled: faltaban algunos (pocos), pero se pudieron rellenar pidiendole al
//           Predictor un estimado -- el resultado es utilizable, pero no 100%
//           dato real.
// - InsufficientData: faltaba tanto que no vale la pena ni intentar rellenar
//           -- el resultado NO trae datos utilizables. Quien reciba esto debe
//           tratar el ciclo como si no hubiera llegado informacion (igual que
//           un ApiError de Metricas, pero por CALIDAD del dato, no por fallo
//           de red).
enum class ValidationStatus { Complete, Filled, InsufficientData };

template <typename T>
struct ValidationResult {
    ValidationStatus status = ValidationStatus::Complete;
    T value{}; // dato modificado
};

// El Validator se sienta ENTRE Metricas y Analytics en el pipeline:
//
//   Metricas (fetch crudo) -> Validator (revisa y rellena huecos) -> Analytics (p95, umbrales)
//
// Su unica responsabilidad es decidir que tan confiable es lo que llego de
// Metricas, y si vale la pena rellenar los huecos usando el Predictor. NO sabe
// nada de CloudWatch ni de AWS (eso es de Metricas), y NO calcula p95 ni
// umbrales de escalado (eso es de Analytics) -- solo se preocupa por la
// completitud del dato.
class Validator {
public:

    // predictor: a quien pedirle estimados para rellenar huecos.
    // completenessThreshold: fraccion minima de instancias con dato real (entre
    //   0 y 1) para confiar en el ciclo. Ej: 0.5 significa "si falta la mitad
    //   o mas de las instancias, no confies en este ciclo, ni lo intentes rellenar".
    Validator(std::shared_ptr<IValuePredictor> predictor, double completenessThreshold = 0.5);

    // Revisa el snapshot "actual" de CPU que devolvio Metricas (rawCurrentCpus),
    // que puede venir INCOMPLETO: si una instancia de `ids` no tiene dato este
    // ciclo, simplemente no aparece como llave en values, del FetchResult (o sea, en rawCurrentCpu)
    // -> recordar que vienen de funciones getCurrent...(), en estas, se obtiene timestamp media de un window pequeño
    // entonces las instancias que no lo tengan, no se añaden al retorno.
    //
    // cpuHistory se usa SOLO para rellenar huecos: si a la instancia "i-123" le
    // falta el dato actual, buscamos su propio historico en cpuHistory["i-123"]
    // y le pedimos al Predictor que estime el valor en targetTimestamp a partir
    // de esa serie -- nunca usamos el historico de OTRA instancia para rellenar,
    // cada maquina tiene su propia carga.

    // ids: Vector con ids de instancias
    // rawCurrentCPU: map con las values para "cada" instancia (las que tenian valor para timestamp)
    // cpuHistory: unordered_map<std::string, MetricSeries> -> history de cada instancia 
    // targetTimestamp: El timestamp 'media' que obtuvimos, y el cual predeciremos en caso de ser necesario
    ValidationResult<std::unordered_map<std::string, double>> ValidateCurrentCpus(
        const std::vector<std::string>& ids,
        const std::unordered_map<std::string, double>& rawCurrentCpu,
        const MetricSeriesByInstance& cpuHistory,
        std::chrono::system_clock::time_point targetTimestamp); 

    // Revisa y rellena el HISTORICO completo de CPU (no un solo instante) --
    // lo necesita Analytics para calcular el p95 entre instancias en cada
    // bucket temporal, y lo que Reactivo/Proactivo reciben depende de que este
    // historico este razonablemente completo a lo largo de la ventana.
    //
    // window: cuanto historico hacia atras se espera (debe cubrir al menos
    //   la ventana mas larga que use Reactivo/Proactivo -- lowWindow u
    //   horizonte, lo que sea mayor) -> porque siempre que analicemos historico, será de horizonte, 
    //   pero si este es menor que ..Window, se toma el mayor
    
    // period: cada cuanto se espera un punto (ej. 60s, el mismo periodo de
    //   consulta que usa Metricas) -- se usa para saber que instantes
    //   "deberian" tener dato, y asi detectar huecos puntuales.
    ValidationResult<MetricSeriesByInstance> ValidateCpuHistory(
        const std::vector<std::string>& ids,
        const MetricSeriesByInstance& rawCpuHistory,
        std::chrono::system_clock::time_point now,
        std::chrono::seconds window,
        std::chrono::seconds period);

private:
    std::shared_ptr<IValuePredictor> _predictor;
    double _completenessThreshold;
};