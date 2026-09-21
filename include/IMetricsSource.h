#pragma once

#include <unordered_map>
#include <string>
#include <vector>
#include <chrono>

struct MetricSample {
    std::chrono::system_clock::time_point timestamp;
    double value; // puede representar un procentaje (cpu) o un numero entero (requests promedio -> 1000 -> 1000 + 1000 + 1000 / 3)
    };

// Contiene la series de tiempo de: o una instancia en un rango de tiempo (cpu de instancia A de 9 a 10)
// o metricas promedio de las instancias en rango de tiempo (request promedio de todas las instancias, de 9 a 10)
using MetricSeries = std::vector<MetricSample>;


// Series de tiempo separadas por instancia. Se usa para CPU: Analytics necesita
// los valores de TODAS las instancias en cada instante para calcular el p95
// entre instancias -- si esto fuera una sola serie ya mezclada, esa informacion
// se pierde antes de llegar a Analytics.
// id:string
// --- 
// INVARIANTE: siempre viene ordenada ASCENDENTE por timestamp (el mas viejo en
// front(), el mas nuevo en back()). AWSMetricsSource la garantiza pidiendole a
// CloudWatch ScanBy=TimestampAscending explicitamente (el default de la API es
// al reves: TimestampDescending). Cualquier otro codigo que construya un
// MetricSeries (ej. al rellenar huecos) debe respetar este orden.
using MetricSeriesByInstance = std::unordered_map<std::string, MetricSeries>;

// Indica si la LLAMADA a AWS tuvo exito o no. Esto es independiente de si el
// valor devuelto esta "completo": una llamada puede ser Ok y aun asi traer
// datos parciales (ej. una instancia sin punto reciente) -- esa incompletitud
// la evalua el Validator, no este tipo.

// enum nos permite definir un tipo segun un conjunto de constantes. enum class es una version
// más segura de enum , pues obliga a especificar de donde viene el tipo -> FetchStatus::Ok
enum class FetchStatus { Ok, ApiError };
 
// templates allow us to write generic code (funct, structs, etc.) that works with
// different data types without rewriting the same logic for each type.
template <typename T>
struct FetchResult {
    FetchStatus status = FetchStatus::Ok; // valor por default es OK
    T value{}; // value{} es para que se inicialice con valor por defecto de ese tipo T que le pasen
 
    bool IsOk() const { return status == FetchStatus::Ok; }
    // con conts indicamos que esta función solamente consulta el objeto -> no puede modificarlo
};


class IMetricsSource{
public:
    
    // hereda a sus hijos destructor.
    virtual ~IMetricsSource() = default;

    // Ids de las instancias actualmente InService en el ASG.
    virtual FetchResult<std::vector<std::string>> GetInstanceIds() = 0;

    // retorna mapa (id_instancia, cpu usage) de cpus en tiempo actual -> por eso no usamos
    // metricsample, porque timestamp es el actual
    virtual FetchResult<std::unordered_map<std::string, double>> GetCurrentCpus(
        const std::vector<std::string>& ids) = 0;

    // importa timestamp,value e id -> <id, metricSample>
    // Historico de CPU de los ultimos `window`, separado por instancia.

    // Ultima lectura de CPU por instancia (id -> % CPU). Instancias sin dato
    // reciente quedan fuera del mapa -- el Controller lo trata como metrica faltante,
    // no como 0%.
    virtual FetchResult<MetricSeriesByInstance> GetCpuHistory(
        const std::vector<std::string>& ids,
        std::chrono::seconds window) = 0;

    // Requests es una metrica del target group completo (RequestCountPerTarget
    // promedio), no por instancia individual -- por eso estos dos no reciben `ids`.

    // request retorna un promedio por TG en tiempo actual -> int
    virtual FetchResult<double> GetCurrentRequest() = 0;

    // sería metricseries porque es un vector de (timestamp, value_prom)
    virtual FetchResult<MetricSeries> GetRequestHistory(std::chrono::seconds window) = 0;
};