#pragma once
// TODO: organizar los include de todos los .h
#include <cstddef>
#include <memory>
//#include <unordered_map>
#include <string>
#include <nlohmann/json.hpp>
#include <vector>
//#include <chrono>
#include "IMetricsSource.h"
#include "IValuePredictor.h"
#include "Validator.h"
#include "ProactiveAnalyzer.h"
#include "ReactiveAnalyzer.h"

// Indica estado de controller
// Idle es revisando datos y tomando decisión
// Acting es cuando estamos haciendo llamadas a AWS, luego de tomar decisión
// Cooling es cuando la acción tuvo exito, y estamos en tiempo de cool down.
enum class State {Idle, Acting, Cooling};

class Controller {
public:
    //  No se le pasa nada, atributos se leen de archivo config.json
    Controller(std::string configFile);
    

    // Ciclo completo
    void Controller::LifeCycle();

private:
    nlohmann::json _config;
    State _state;
    bool _falseData; // porque lo necesito para varias funciones, esta en config file
    std::string _asg;
    std::string _targetGroupArn;
    std::vector<std::string> _instanceIds;
    std::unique_ptr<IMetricsSource> _metricsSource; // Unico porque su dueño solo será controller, los otros le pediran algun dato, pero no serán dueños
    std::unique_ptr<IMetricsSource> _idsSource; // de donde se toman los ids, siempre es de awsmetrics
    FakeMetricsSource* _fakeMetricsSource; // Porque no deberia existir si metricsource no lo hace -> no es un shard_pointer, sino un pointer normal, que no es dueño
    
    
    int _minInstances;
    int _maxInstances;
    
    double _highThreshold;
    double  _lowThreshold;
    
    std::chrono::seconds _historyWindow; // Window usada para pedir historial -> debe ser lo suficientemente grande
    // para que historial sirva a predictive y a proactive
    std::chrono::seconds _horizonWindow; // Horizonte sobre el cual proactive predicirá -> >= tiempo de acción + margen
    std::chrono::seconds _sustainedHighWindow; // Window que metrica debe estar por encima de highthreshold para que reactive actue -> La 
    // implementamos (el sustained..Window en general) para evitar reaccionar a picos puntuales, que no servirá reaccionar porque para cuando se 
    // cree instancia, ya abrá pasado
    std::chrono::seconds _sustainedLowWindow; // Window que metrica debe estar por debajo de lowthreshold para que reactive actue 




    std::chrono::seconds _queryPeriod; // el mismo periodo de muestreo que usa Metricas (ej. 60s)
    
    std::chrono::seconds _scaleOutCooldown;
    std::chrono::seconds _scaleInCooldown;
    
    std::mt19937 _rng; // para cpuoverride, generar valores
    
    // Objetos externos necesarios
    std::shared_ptr<IValuePredictor> _valuePredictor; // Debe ser shared porque  dos objetos necesitan ser dueños del mismo objeto. -> ProactiveAnalizer, que recibe un shared_ptr, y Analizer, que recibe tambien uno
    // Ambos trabajan con un predictor
    Validator _validator;
    ProactiveAnalyzer _proactive;
    ReactiveAnalyzer _reactive;
    
    // TODO: Ponerlas publicas o privadas? 
    nlohmann::json LoadConfig(const string& configFile);

    bool GetInstanceIds();
    
    FetchResult<CurrentCpuSnapshot> GetCurrentCpus();
    FetchResult<double> GetCurrentRequest();

    void Evaluate();

    // Útil porque despues de añadir o quitar instancias, se debe actulizar el _instanceIds
    void SetInstanceIds(std::vector<std::string> newIds);

};