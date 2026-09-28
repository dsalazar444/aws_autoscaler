#include <aws/core/Aws.h>
#include <aws/autoscaling/AutoScalingClient.h>
#include <aws/monitoring/CloudWatchClient.h>
#include <aws/ec2/EC2Client.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <stdexcept>
#include <string>

#include "Controller.h"
#include "LogGraph.h"

#include <chrono>
#include <thread>
#include <iostream>

using json = nlohmann::json;

int main()
{
    // Inicializar AWS SDK
    Aws::SDKOptions options;
    Aws::InitAPI(options);

    // Abrir archivo de configuración
    std::string configFile = "../config.json";
        
    std::ifstream f(configFile); // crea objeto que abre archivo

    if (!f.is_open()) {
        throw std::runtime_error("Could not open config file: " + configFile);
    }

    // cTraemos su contenido
    json configData = json::parse(f);


    {
        Controller controller("../config.json");

        constexpr int TEST_DURATION_MINUTES = 10;

        for (int minute = 0; minute < TEST_DURATION_MINUTES; ++minute) {

            std::cout << "\n=== Ciclo " << minute + 1
                      << "/" << TEST_DURATION_MINUTES
                      << " ===\n";

            // Ejecuta un ciclo completo del autoscaler, pasandole timestamp actual
            controller.LifeCycle(std::chrono::system_clock::now());

            // Esperar hasta el siguiente ciclo
            std::this_thread::sleep_for(
                std::chrono::minutes(1)
            );
        }

        // Cuando terminan los 10 minutos,
        // leer el JSON y generar la gráfica
        LogGraph graph(
            std::filesystem::path(configData["logPathFile"].get<std::string>()), // Mismo archivo donde registramos datos
            std::filesystem::path(configData["logGraphics"].get<std::string>())
        );

        graph.Generate();
    }

    Aws::ShutdownAPI(options);

    return 0;
}