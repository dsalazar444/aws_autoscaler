#pragma once

#include <filesystem>

class LogGraph {
public:
    LogGraph(
        std::filesystem::path logFilePath,
        std::filesystem::path outputFilePath
    );

    // Lee los logs y genera una gráfica SVG con:
    // - CPU global
    // - Requests globales
    // - Cantidad de instancias
    
    void Generate();

private:
    std::filesystem::path _logFilePath;
    std::filesystem::path _outputFilePath;
};
