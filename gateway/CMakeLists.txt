cmake_minimum_required(VERSION 3.16)

project(agro_gateway VERSION 0.1.0 LANGUAGES CXX)

option(BUILD_TESTING "Build gateway tests" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PahoMqttCpp REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(Threads REQUIRED)
find_package(CURL REQUIRED)

add_library(telemetry_parser
    src/TelemetryParser.cpp
)

target_include_directories(telemetry_parser PUBLIC include)
target_link_libraries(telemetry_parser PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(telemetry_parser PRIVATE -Wall -Wextra -Wpedantic)

add_library(persistence_core
    src/LineProtocolSerializer.cpp
    src/PersistenceQueue.cpp
)

target_include_directories(persistence_core PUBLIC include)
target_link_libraries(persistence_core PUBLIC Threads::Threads)
target_compile_options(persistence_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(agro-gateway
    src/InfluxWriter.cpp
    src/main.cpp
    src/MqttClient.cpp
)

target_include_directories(agro-gateway PRIVATE include)
target_link_libraries(agro-gateway
    PRIVATE
        telemetry_parser
        persistence_core
        PahoMqttCpp::paho-mqttpp3
        CURL::libcurl
)
target_compile_options(agro-gateway PRIVATE -Wall -Wextra -Wpedantic)

if(BUILD_TESTING)
    enable_testing()

    add_executable(telemetry_parser_tests
        tests/TelemetryParserTests.cpp
    )
    target_link_libraries(telemetry_parser_tests PRIVATE telemetry_parser)
    target_compile_options(telemetry_parser_tests PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME telemetry_parser_tests COMMAND telemetry_parser_tests)

    add_executable(persistence_core_tests
        tests/PersistenceCoreTests.cpp
    )
    target_link_libraries(persistence_core_tests PRIVATE persistence_core)
    target_compile_options(persistence_core_tests PRIVATE -Wall -Wextra -Wpedantic)

    add_test(NAME persistence_core_tests COMMAND persistence_core_tests)
endif()
