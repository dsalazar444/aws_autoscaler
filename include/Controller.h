#pragma once
// TODO: organizar los include de todos los .h
#include <cstddef>
#include <memory>
//#include <unordered_map>
#include <string>
#include <nlohmann/json.hpp>
#include <vector>
#include <optional>

//#include <chrono>
#include "IMetricsSource.h"
#include "IValuePredictor.h"
#include "Validator.h"
#include "ProactiveAnalyzer.h"
#include "ReactiveAnalyzer.h"
#include "AWSActioner.h"
#include "DecidedAction.h"
#include "Logger.h"

// Indica estado de controller
// Idle es revisando datos y tomando decisión
// Acting es cuando estamos haciendo llamadas a AWS, luego de tomar decisión
// Cooling es cuando la acción tuvo exito, y estamos en tiempo de cool down.
enum class State { Idle, CooldownOut, CooldownIn };

class Controller {
public:

    //  No se le pasa nada, atributos se leen de archivo config.json
    Controller(std::string configFile);
    

    // Ciclo completo
    // now ->  para tener instante en que se ejecuta ciclo, y mantenerlo
    // lastAction -> porque cooldown depende de qué se hizo anteriormente
    void Controller::LifeCycle(std::chrono::system_clock::time_point now);

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
    
    double _highThresholdCpu;
    double _lowThresholdCpu;

    int _highThresholdReq;
    int _lowThresholdReq;
    
    std::chrono::seconds _historyWindow; // Window usada para pedir historial -> debe ser lo suficientemente grande
    // para que historial sirva a predictive y a proactive
    std::chrono::seconds _horizonWindow; // Horizonte sobre el cual proactive predicirá -> >= tiempo de acción + margen

    std::chrono::seconds _sustainedHighWindowCpu; // Window que metrica debe estar por encima de highthreshold para que reactive actue -> La 
    // implementamos (el sustained..Window en general) para evitar reaccionar a picos puntuales, que no servirá reaccionar porque para cuando se 
    // cree instancia, ya abrá pasado
    std::chrono::seconds _sustainedLowWindowCpu; // Window que metrica debe estar por debajo de lowthreshold para que reactive actue 

    std::chrono::seconds _sustainedHighWindowReq;

    std::chrono::seconds _queryPeriod; // el mismo periodo de muestreo que usa Metricas (ej. 60s)
    
    std::chrono::seconds _scaleOutCooldown;
    std::chrono::seconds _scaleInCooldown;
    std::chrono::system_clock::time_point _cooldownUntil; // Solo tiene sentido si _state =! Idle

    
    
    std::mt19937 _rng; // para cpuoverride, generar valores
    
    // Objetos externos necesarios
    std::shared_ptr<IValuePredictor> _valuePredictor; // Debe ser shared porque  dos objetos necesitan ser dueños del mismo objeto. -> ProactiveAnalizer, que recibe un shared_ptr, y Analizer, que recibe tambien uno
    // Ambos trabajan con un predictor
    Validator _validator;
    ProactiveAnalyzer _proactive;
    ReactiveAnalyzer _reactive;
    AWSActioner _actioner;
    Logger _logger;
    
    // TODO: Ponerlas publicas o privadas? 
    nlohmann::json LoadConfig(const string& configFile);

    bool GetInstanceIds();
    
    FetchResult<CurrentCpuSnapshot> GetCurrentCpus();
    FetchResult<double> GetCurrentRequest();

    void BuildAndSendRecord(std::chrono::system_clock::time_point now, optional<std::chrono::seconds> window,
                int instanceCount, std::optional<double> currentGlobalCpu,
                std::optional<double> currentGlobalReq, const std::string& justification,
                std::optional<ReactiveSignal> reactiveSignalCpu, std::optional<ReactiveSignal> reactiveSignalReq,
                std::optional<ProactiveSignal> proactiveSignal,
                std::optional<double> proactiveEstimatedCpu, int targetCount,
                std::optional<std::string> idToDelete, Action decision, 
                std::optional<ActionResult> actionResult);

    void EvaluateAndDecide(const MetricSeries& globalCpusHistory,
                ValidationResult<unordered_map<string, double>> validatedCurrentCpu,
                const MetricSeries& historyRequest,
                std::chrono::system_clock::time_point now,
                optional<double> currentGlobalCpu,
                FetchResult<double> currentRequest,
                int totalActualInstances);

    scaleAction::DecidedAction Decide(const ProactiveEvaluation proactiveOpinion, 
            const ReactiveSignal reactiveOpinionCpu,
            const ReactiveSignal reactiveOpinionReq,
            std::optional<double> currentGlobalCpu,
            ValidationResult<std::unordered_map<std::string, double>> validatedCurrentCpu,
            int totalActualInstances);

    std::optional<ActionResult> Act(scaleAction::DecidedAction action);

    
    int CalculateMinimumSafeInstanceCount(int currentCount, double referenceCpu) const;
    
    std::string FindLeastLoadedInstance(const std::unordered_map<std::string, double>& validatedCurrentCpu) const;

    // Útil porque despues de añadir o quitar instancias, se debe actulizar el _instanceIds
    void SetInstanceIds(std::vector<std::string> newIds);

};