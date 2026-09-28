#pragma once

#include <chrono>
#include <filesystem>
#include <optional>
#include <string>

#include "ReactiveAnalyzer.h"
#include "ProactiveAnalyzer.h"
#include "DecidedAction.h"
#include "AWSActioner.h"

using namespace scaleAction; // de DecidedAction

class Logger{
public:

    Logger(std::filesystem::path filePath);

    // Todo lo que el taller pide que quede registrado por ciclo: timestamp,
    // metricas observadas, capacidad existente, decision, justificacion, accion
    // ejecutada y resultado. Un DecisionRecord se genera SIEMPRE, incluso cuando
    // la decision es MaintainCapacity -- "no hacer nada" tambien es una decision
    // que debe quedar explicada.
    struct DecisionRecord {
        std::chrono::system_clock::time_point timestamp; // Time at wich decision was made
        std::chrono::seconds epochSeconds; // Otro formato de timestamp para facilitar busqueda

        // Metrics and observation interval considered 
        std::string metricsConsidered = "CPU y Requests";
        optional<std::chrono::seconds> analyzedWindow{0};  // el historyWindow configurado en Controller
        
        // Existing state
        int instanceCountBefore = 0;
        std::optional<double> currentGlobalCpu;  // nullopt si no se pudo calcular este ciclo
        std::optional<double> currentGlobalReq;  // nullopt si no se pudo calcular este ciclo

        // Decisions by modules
        std::optional<ReactiveSignal> reactiveSignalCpu = ReactiveSignal::InsufficientData; // optional porque hay logs que se registran antes de siquiera llamarlos
        std::optional<ReactiveSignal> reactiveSignalReq = ReactiveSignal::InsufficientData; // optional porque hay logs que se registran antes de siquiera llamarlos
        std::optional<ProactiveSignal> proactiveSignal = ProactiveSignal::Unknown;
        std::optional<double> proactiveEstimatedCpu;
        int targetCount; // cantidad de MVs que quedarian en TOTAL segun acción.
        std::optional<std::string> idToDelete; // Si se decidió eliminar, cuál.

        
        // Decided action
        Action decision = Action::Mantain;
        std::string justification;
    
        // result of said action
        // nullopt cuando la decision fue MaintainCapacity -- no se ejecuto
        // ninguna accion sobre AWS, asi que no hay resultado que registrar
        std::optional<ActionResult> actionResult;
    };

    void Log(const DecisionRecord& decisionRecord);

private:
    std::filesystem::path _filePath; // /logs/log.json
//TODO: Revisar los destructores en todos -> en Interfaces son ~I() = Default, no sé sin I como será

    void Logger::LogTerminal(const DecisionRecord& decisionRecord);
    void Logger::LogJSON(const DecisionRecord& decisionRecord);
};