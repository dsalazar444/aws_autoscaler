#pragma once

#include <unordered_map>
#include <iostream>
#include <vector>
#include <chrono>


class IMetricsSource{
public:

    // struct necesario porque todas instancias lo necesitan. y publico porque es retornado por una función publica
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
    using MetricSeriesByInstance = std::unordered_map<std::string, MetricSeries>;

    virtual ~IMetricsSource() = default;

    // Ids de las instancias actualmente InService en el ASG.
    virtual std::vector<std::string> GetInstanceIds() = 0;

    // retorna mapa (id_instancia, cpu usage) de cpus en tiempo actual -> por eso no usamos
    // metricsample, porque timestamp es el actual
    virtual std::unordered_map<std::string, double> GetCurrentCpus(
        const std::vector<std::string>& ids) = 0;

    // importa timestamp,value e id -> <id, metricSample>
    // Historico de CPU de los ultimos `window`, separado por instancia.

    // Ultima lectura de CPU por instancia (id -> % CPU). Instancias sin dato
    // reciente quedan fuera del mapa -- el Controller lo trata como metrica faltante,
    // no como 0%.
    virtual MetricSeriesByInstance GetCpuHistory(
        const std::vector<std::string>& ids,
        std::chrono::seconds window) = 0;

    // Requests es una metrica del target group completo (RequestCountPerTarget
    // promedio), no por instancia individual -- por eso estos dos no reciben `ids`.

    // request retorna un promedio por TG en tiempo actual -> int
    virtual int GetCurrentRequest() = 0;

    // sería metricseries porque es un vector de (timestamp, value_prom)
    virtual MetricSeries GetRequestHistory(std::chrono::seconds window) = 0;
};