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
    src/main.cpp
)

target_link_libraries(aws-autoscaler
    PRIVATE
        ${AWSSDK_LIBRARIES}
        nlohmann_json::nlohmann_json
)