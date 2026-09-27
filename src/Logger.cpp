#include "Logger.h"

#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>

using namespace std;

// Conversion a texto de los enums de DecisionRecord -- compartida entre
// cualquier logger concreto (JSONL, consola, lo que venga despues) para no
// duplicar el mapeo enum->string en cada uno.
namespace {
    std::string ToString(Action action) {
        switch (action) {
            case Action::Mantain:
                return "MAINTAIN_CAPACITY";
            case Action::Increment:
                return "INCREASE_CAPACITY";
            case Action::Decrement:
                return "REDUCE_CAPACITY";
        }
        return "UNKNOWN";  // defensivo -- no deberia alcanzarse
    }
    
    std::string ToString(ReactiveSignal signal) {
        switch (signal) {
            case ReactiveSignal::SustainedHigh:
                return "SUSTAINED_HIGH";
            case ReactiveSignal::SustainedLow:
                return "SUSTAINED_LOW";
            case ReactiveSignal::Normal:
                return "NORMAL";
            case ReactiveSignal::InsufficientData:
                return "INSUFFICIENT_DATA";
        }
        return "UNKNOWN";
    }
    
    std::string ToString(ProactiveSignal signal) {
        switch (signal) {
            case ProactiveSignal::PredictsAboveHighThreshold:
                return "PREDICTS_ABOVE_HIGH_THRESHOLD";
            case ProactiveSignal::PredictsSafe:
                return "PREDICTS_SAFE";
            case ProactiveSignal::Unknown:
                return "UNKNOWN";
        }
        return "UNKNOWN";
    }
    
    std::string ToString(ActionResult result) {
        switch (result) {
            case ActionResult::Success:
                return "SUCCESS";
            case ActionResult::Failed:
                return "FAILED";
        }
        return "UNKNOWN";
    }

    std::string FormatTimestamp(std::chrono::system_clock::time_point timestamp) {
        const auto time = std::chrono::system_clock::to_time_t(timestamp);

        std::tm tm{};
        localtime_r(&time, &tm);

        std::ostringstream oss;
        oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");

        return oss.str();
    }

    // valor o "null" -- para los campos opcionales que pueden faltar segun en
    // que punto del ciclo se genero el registro (ej. currentGlobalCpu si fallo
    // Metricas antes de poder calcularlo)
    template <typename T>
    std::string optionalToString(const std::optional<T>& value) {
        if (!value.has_value()) {
            return "null";
        }

        std::ostringstream oss;

        if constexpr (std::is_same_v<T, std::string>) {
            oss << "\"" << *value << "\""; // Porque sin esto, si llega string como "Hola", oss << le quita las comillas, y json ya no lo reconocería como string valido
        } else {
            oss << *value;
        }

        return oss.str();
    }
}

// TODO: Revisar si usar, cuando haya namespace, std, porque pueden haber ambiguedades
Logger::Logger(std::filesystem::path filePath): _filePath(std::move(filePath)) {
    // aseguramos que la carpeta contenedora exista -- best-effort, igual que
    // en FakeMetricsSource
    error_code ignored;
    filesystem::create_directories(_filePath.parent_path(), ignored);
};

void Logger::Log(const DecisionRecord& decisionRecord){
    LogTerminal(decisionRecord);
    LogJSON(decisionRecord);
}

void Logger::LogTerminal(const DecisionRecord& record){

    auto time = std::chrono::system_clock::to_time_t(record.timestamp);

    cout << "[" << FormatTimestamp(record.timestamp) << "] "
         << "actualInstances=" << record.instanceCountBefore << " "
         << "decisionMade=" << ToString(record.decision) << endl;
    
    cout << "Actual metrics used to make decision: \n" 
         << "globalCpu=" <<  optionalToString(record.currentGlobalCpu) << "%\n"
         << "globalRequest= " << optionalToString(record.currentGlobalReq);
    
        //TODO: Poner request

    cout << "Modules signals: \n" 
         << "reactiveSignal: " << ToString(record.reactiveSignal) << ",\n"
         << "proactiveSignal:" << ToString(record.proactiveSignal) << ",\n"
         << "proactiveEstimatedCpu:" << optionalToString(record.proactiveEstimatedCpu) << ",\n"
         << "targetCount:" << record.targetCount << ",\n"
         << "instanceToDelete:" << optionalToString(record.idToDelete) << endl;


    cout << "decisionJustificaciton: " << record.justification << endl;
    cout << "actionResult=" << optionalToString(record.actionResult);

    std::cout << " -- " << record.justification << "\n";
}

void Logger::LogJSON(const DecisionRecord& record){

    std::ofstream file(_filePath, std::ios::app);
    if (!file) {
        // best-effort: si no se puede escribir el log, preferimos que el
        // ciclo de Controller siga corriendo antes que detener el sistema
        // completo por un problema de disco -- pero este ciclo queda sin
        // registro de auditoria, vale la pena monitorear esto aparte
        return;
    }

    auto epochSeconds = std::chrono::duration_cast<std::chrono::seconds>(record.timestamp.time_since_epoch()).count();

    file << "{"
         << "\"timestamp\":" << FormatTimestamp(record.timestamp) << ","
         << "\"epochSeconds\":" << epochSeconds << ","

         << "\"metricsConsidered\":" << record.metricsConsidered << ","
         << "\"analyzedWindowSeconds\":" << record.analyzedWindow.count() << ","

         << "\"instanceCountBefore\":" << record.instanceCountBefore << ","
         << "\"currentGlobalCpu\":" << optionalToString(record.currentGlobalCpu) << ","
         << "\"currentGlobalReq\":" << optionalToString(record.currentGlobalReq) << ","

         << "\"reactiveSignal\":\"" << ToString(record.reactiveSignal) << "\","
         << "\"proactiveSignal\":\"" << ToString(record.proactiveSignal) << "\","
         << "\"proactiveEstimatedCpu\":" << optionalToString(record.proactiveEstimatedCpu) << ","
         << "\"targetCount\":" << record.targetCount << ","
         << "\"instanceToDelete\":" << optionalToString(record.idToDelete) << ","

         << "\"decision\":\"" << ToString(record.decision) << "\","
         << "\"justification\":\"" << record.justification << "\","
         << "\"actionResult\":" << optionalToString(record.actionResult) << "\","
         << "}\n";
}
