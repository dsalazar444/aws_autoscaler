#include "Logger.h"

#include <nlohmann/json.hpp> 
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>

using json = nlohmann::json;
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
    
    std::string ToString(std::optional<ReactiveSignal> signal) {

        if (!signal.has_value()){
            return "null";
        }

        switch (signal.value()) {
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
    
    std::string ToString(std::optional<ProactiveSignal> signal) {
        
        if (!signal.has_value()){
            return "null";
        }

        switch (signal.value()) {
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
    // Util solo para tipos de datos comunes, para datos propios se debe usar método propio (por ejm: toString de Reactive y Proactive signals)
    template <typename T>
    std::string optionalToJson(const std::optional<T>& value) {
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

    std::string optionalWindowToJson(const std::optional<std::chrono::seconds> window){
        if (!window.has_value()){
            return "null";
        }

        return std::to_string(window.value().count());
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
         << "globalCpu=" <<  optionalToJson(record.currentGlobalCpu) << "%\n"
         << "globalRequest= " << optionalToJson(record.currentGlobalReq);

    cout << "Modules signals: \n" 
         << "reactiveSignalCpu: " << ToString(record.reactiveSignalCpu) << ",\n"
         << "reactiveSignalReq: " << ToString(record.reactiveSignalReq) << ",\n"
         << "proactiveSignal:" << ToString(record.proactiveSignal) << ",\n"
         << "proactiveEstimatedCpu:" << optionalToJson(record.proactiveEstimatedCpu) << ",\n"
         << "targetCount:" << record.targetCount << ",\n"
         << "instanceToDelete:" << optionalToJson(record.idToDelete) << endl;


    cout << "decisionJustificaciton: " << record.justification << endl;
    cout << "actionResult=" << optionalToJson(record.actionResult);

    std::cout << " -- " << record.justification << "\n";
}

void Logger::LogJSON(const DecisionRecord& record)
{
    std::ofstream file(_filePath, std::ios::app);

    if (!file) {
        std::cerr << "No se pudo abrir el archivo JSON\n";
        return;
    }

    const auto epochSeconds =std::chrono::duration_cast<std::chrono::seconds>(record.timestamp.time_since_epoch()).count();

    json logEntry = {
        {"timestamp", FormatTimestamp(record.timestamp)},
        {"epochSeconds", epochSeconds},

        {"metricsConsidered", record.metricsConsidered},
        {"analyzedWindowSeconds",
            record.analyzedWindow.has_value()
                ? json(record.analyzedWindow->count())
                : json(nullptr)},

        {"instanceCountBefore", record.instanceCountBefore},

        {"currentGlobalCpu",
            record.currentGlobalCpu.has_value()
                ? json(*record.currentGlobalCpu)
                : json(nullptr)},

        {"currentGlobalReq",
            record.currentGlobalReq.has_value()
                ? json(*record.currentGlobalReq)
                : json(nullptr)},

        {"reactiveSignalCpu", ToString(record.reactiveSignalCpu)},
        {"reactiveSignalReq", ToString(record.reactiveSignalReq)},
        {"proactiveSignal", ToString(record.proactiveSignal)},

        {"proactiveEstimatedCpu",
            record.proactiveEstimatedCpu.has_value()
                ? json(*record.proactiveEstimatedCpu)
                : json(nullptr)},

        {"targetCount", record.targetCount},

        {"instanceToDelete",
            record.idToDelete.has_value()
                ? json(*record.idToDelete)
                : json(nullptr)},

        {"decision", ToString(record.decision)},
        {"justification", record.justification},

        {"actionResult",
            record.actionResult.has_value()
                ? json(ToString(*record.actionResult))
                : json(nullptr)}
    };

    file << logEntry.dump() << '\n';
}