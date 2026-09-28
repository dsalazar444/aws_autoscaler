cmake_minimum_required(VERSION 3.20)

project(aws-autoscaler LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(AWSSDK REQUIRED COMPONENTS
    core
    autoscaling
    monitoring
    ec2
)
find_package(nlohmann_json CONFIG REQUIRED)

add_executable(aws-autoscaler
    src/Analytics.cpp
    src/AWSActioner.cpp
    src/AWSMetricsSource.cpp
    src/Controller.cpp
    src/FakeMetricsSource.cpp
    src/LinearRegressionPredictor.cpp
    src/Logger.cpp
    src/LogGraph.cpp
    src/main.cpp
    src/ProactiveAnalyzer.cpp
    src/ReactiveAnalyzer.cpp
    src/Validator.cpp
)

target_include_directories(aws-autoscaler
    PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include)

target_link_libraries(aws-autoscaler
    PRIVATE
        ${AWSSDK_LIBRARIES}
        nlohmann_json::nlohmann_json
)