#include "LogGraph.h"

#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>
#include <string>
#include <iomanip>
#include <algorithm>
#include <cmath>

using json = nlohmann::json;
// using namespace std;
namespace {

struct LogPoint {
    std::string timestamp;
    double cpu;
    double requests;
    double instances;
};

double GetMax(const std::vector<LogPoint>& points, double LogPoint::*member) {
    double maxValue = 0.0;

    for (const auto& point : points) {
        maxValue = std::max(maxValue, point.*member);
    }

    return maxValue;
}

std::string EscapeXml(const std::string& value) {
    std::string result;

    for (char c : value) {
        switch (c) {
            case '&': result += "&amp;"; break;
            case '<': result += "&lt;"; break;
            case '>': result += "&gt;"; break;
            case '"': result += "&quot;"; break;
            case '\'': result += "&apos;"; break;
            default: result += c; break;
        }
    }

    return result;
}

} // namespace


LogGraph::LogGraph(
    std::filesystem::path logFilePath,
    std::filesystem::path outputFilePath
)
    : _logFilePath(std::move(logFilePath)),
      _outputFilePath(std::move(outputFilePath))
{
}


void LogGraph::Generate() {

    std::ifstream file(_logFilePath);

    if (!file) {
        throw std::runtime_error(
            "No se pudo abrir el archivo de logs: "
            + _logFilePath.string()
        );
    }

    std::vector<LogPoint> points;

    std::string line;

    while (std::getline(file, line)) {

        if (line.empty()) {
            continue;
        }

        try {
            json record = json::parse(line);

            // Algunos ciclos pueden no tener métricas.
            if (!record.contains("currentGlobalCpu") ||
                !record.contains("currentGlobalReq")) {
                continue;
            }

            if (record["currentGlobalCpu"].is_null() ||
                record["currentGlobalReq"].is_null()) {
                continue;
            }

            LogPoint point;

            point.timestamp = record["timestamp"].get<std::string>();
            point.cpu = record["currentGlobalCpu"].get<double>();
            point.requests = record["currentGlobalReq"].get<double>();
            point.instances = record["instanceCountBefore"].get<double>();

            points.push_back(point);

        } catch (const json::exception& e) {

            std::cerr
                << "Advertencia: se ignoró una línea inválida del log: "
                << e.what()
                << '\n';
        }
    }

    if (points.empty()) {
        throw std::runtime_error(
            "No hay datos válidos para generar la gráfica."
        );
    }

    constexpr double WIDTH = 1400.0;
    constexpr double HEIGHT = 700.0;

    constexpr double LEFT = 100.0;
    constexpr double RIGHT = 100.0;
    constexpr double TOP = 80.0;
    constexpr double BOTTOM = 120.0;

    const double graphWidth = WIDTH - LEFT - RIGHT;
    const double graphHeight = HEIGHT - TOP - BOTTOM;

    const double maxCpu = std::max(100.0, GetMax(points, &LogPoint::cpu));
    const double maxRequests =
        std::max(1.0, GetMax(points, &LogPoint::requests));

    const double maxInstances =
        std::max(1.0, GetMax(points, &LogPoint::instances));

    auto xPosition = [&](std::size_t index) {

        if (points.size() == 1) {
            return LEFT + graphWidth / 2.0;
        }

        return LEFT +
            (static_cast<double>(index) /
             static_cast<double>(points.size() - 1))
            * graphWidth;
    };

    auto yPosition = [&](double value, double maxValue) {

        return TOP + graphHeight -
            (value / maxValue) * graphHeight;
    };

    std::ofstream output(_outputFilePath);

    if (!output) {
        throw std::runtime_error(
            "No se pudo crear el archivo de gráfica: "
            + _outputFilePath.string()
        );
    }

    output << R"(<?xml version="1.0" encoding="UTF-8"?>)";

    output << "\n<svg xmlns=\"http://www.w3.org/2000/svg\" "
           << "width=\"" << WIDTH
           << "\" height=\"" << HEIGHT
           << "\" viewBox=\"0 0 " << WIDTH
           << " " << HEIGHT << "\">\n";

    // Fondo
    output << R"(<rect width="100%" height="100%" fill="white"/>)"
           << '\n';

    // Título
    output << "<text x=\"" << WIDTH / 2
           << "\" y=\"35\" text-anchor=\"middle\" "
           << "font-family=\"sans-serif\" font-size=\"22\" "
           << "font-weight=\"bold\">"
           << "AWS Autoscaler Metrics"
           << "</text>\n";

    // Ejes
    output << "<line x1=\"" << LEFT
           << "\" y1=\"" << TOP
           << "\" x2=\"" << LEFT
           << "\" y2=\"" << TOP + graphHeight
           << "\" stroke=\"black\"/>\n";

    output << "<line x1=\"" << LEFT
           << "\" y1=\"" << TOP + graphHeight
           << "\" x2=\"" << LEFT + graphWidth
           << "\" y2=\"" << TOP + graphHeight
           << "\" stroke=\"black\"/>\n";

    // Escala CPU - eje izquierdo
    constexpr int GRID_LINES = 5;

    for (int i = 0; i <= GRID_LINES; ++i) {

        const double ratio =
            static_cast<double>(i) / GRID_LINES;

        const double y =
            TOP + graphHeight - ratio * graphHeight;

        const double value = ratio * maxCpu;

        output << "<line x1=\"" << LEFT
               << "\" y1=\"" << y
               << "\" x2=\"" << LEFT + graphWidth
               << "\" y2=\"" << y
               << "\" stroke=\"lightgray\"/>\n";

        output << "<text x=\"" << LEFT - 10
               << "\" y=\"" << y + 5
               << "\" text-anchor=\"end\" "
               << "font-family=\"sans-serif\" font-size=\"12\">"
               << std::fixed << std::setprecision(0)
               << value << "%"
               << "</text>\n";
    }

    // Eje derecho: Requests
    output << "<line x1=\"" << LEFT + graphWidth
           << "\" y1=\"" << TOP
           << "\" x2=\"" << LEFT + graphWidth
           << "\" y2=\"" << TOP + graphHeight
           << "\" stroke=\"black\"/>\n";

    for (int i = 0; i <= GRID_LINES; ++i) {

        const double ratio =
            static_cast<double>(i) / GRID_LINES;

        const double y =
            TOP + graphHeight - ratio * graphHeight;

        const double value = ratio * maxRequests;

        output << "<text x=\"" << LEFT + graphWidth + 10
               << "\" y=\"" << y + 5
               << "\" text-anchor=\"start\" "
               << "font-family=\"sans-serif\" font-size=\"12\">"
               << std::fixed << std::setprecision(0)
               << value
               << "</text>\n";
    }

    // Tercer eje: instancias.
    // Se dibuja ligeramente separado del eje derecho.
    const double instanceAxisX = LEFT + graphWidth + 55;

    output << "<line x1=\"" << instanceAxisX
           << "\" y1=\"" << TOP
           << "\" x2=\"" << instanceAxisX
           << "\" y2=\"" << TOP + graphHeight
           << "\" stroke=\"black\"/>\n";

    for (int i = 0; i <= GRID_LINES; ++i) {

        const double ratio =
            static_cast<double>(i) / GRID_LINES;

        const double y =
            TOP + graphHeight - ratio * graphHeight;

        const double value = ratio * maxInstances;

        output << "<text x=\"" << instanceAxisX + 10
               << "\" y=\"" << y + 5
               << "\" text-anchor=\"start\" "
               << "font-family=\"sans-serif\" font-size=\"12\">"
               << std::fixed << std::setprecision(0)
               << value
               << "</text>\n";
    }

    // Líneas de las tres métricas
    std::ostringstream cpuPath;
    std::ostringstream requestPath;
    std::ostringstream instancePath;

    for (std::size_t i = 0; i < points.size(); ++i) {

        const auto& point = points[i];

        const double x = xPosition(i);

        const double cpuY =
            yPosition(point.cpu, maxCpu);

        const double requestY =
            yPosition(point.requests, maxRequests);

        const double instanceY =
            yPosition(point.instances, maxInstances);

        if (i == 0) {
            cpuPath << "M ";
            requestPath << "M ";
            instancePath << "M ";
        } else {
            cpuPath << " L ";
            requestPath << " L ";
            instancePath << " L ";
        }

        cpuPath << x << " " << cpuY;
        requestPath << x << " " << requestY;
        instancePath << x << " " << instanceY;
    }

    // CPU
    output << "<path d=\"" << cpuPath.str()
           << "\" fill=\"none\" stroke=\"red\" "
           << "stroke-width=\"2\"/>\n";

    // Requests
    output << "<path d=\"" << requestPath.str()
           << "\" fill=\"none\" stroke=\"blue\" "
           << "stroke-width=\"2\"/>\n";

    // Instances
    output << "<path d=\"" << instancePath.str()
           << "\" fill=\"none\" stroke=\"green\" "
           << "stroke-width=\"2\"/>\n";

    // Eje X: timestamps.
    //
    // No mostramos todos los timestamps porque podrían
    // ser demasiados y terminarían superpuestos.
    const std::size_t maxLabels = 10;
    const std::size_t step =
        std::max<std::size_t>(
            1,
            points.size() / maxLabels
        );

    for (std::size_t i = 0; i < points.size(); i += step) {

        const double x = xPosition(i);
        const double y = TOP + graphHeight + 25;

        output << "<text x=\"" << x
               << "\" y=\"" << y
               << "\" text-anchor=\"middle\" "
               << "font-family=\"sans-serif\" "
               << "font-size=\"11\" "
               << "transform=\"rotate(30 "
               << x << " " << y << ")\">"
               << EscapeXml(points[i].timestamp)
               << "</text>\n";
    }

    // Leyenda
    const double legendY = HEIGHT - 25;

    output << "<line x1=\""
           << LEFT
           << "\" y1=\"" << legendY
           << "\" x2=\"" << LEFT + 30
           << "\" y2=\"" << legendY
           << "\" stroke=\"red\" stroke-width=\"3\"/>\n";

    output << "<text x=\"" << LEFT + 40
           << "\" y=\"" << legendY + 5
           << "\" font-family=\"sans-serif\" "
           << "font-size=\"13\">CPU global (%)</text>\n";

    output << "<line x1=\""
           << LEFT + 200
           << "\" y1=\"" << legendY
           << "\" x2=\"" << LEFT + 230
           << "\" y2=\"" << legendY
           << "\" stroke=\"blue\" stroke-width=\"3\"/>\n";

    output << "<text x=\"" << LEFT + 240
           << "\" y=\"" << legendY + 5
           << "\" font-family=\"sans-serif\" "
           << "font-size=\"13\">Requests</text>\n";

    output << "<line x1=\""
           << LEFT + 370
           << "\" y1=\"" << legendY
           << "\" x2=\"" << LEFT + 400
           << "\" y2=\"" << legendY
           << "\" stroke=\"green\" stroke-width=\"3\"/>\n";

    output << "<text x=\"" << LEFT + 410
           << "\" y=\"" << legendY + 5
           << "\" font-family=\"sans-serif\" "
           << "font-size=\"13\">Instancias</text>\n";

    output << "</svg>\n";

    std::cout
        << "Grafica generada en: "
        << _outputFilePath
        << '\n';
}

