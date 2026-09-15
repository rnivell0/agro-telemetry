#pragma once

#include <cstdint>

/**
 * Una medición completa transferida por valor a través de la cola.
 *
 * Solo contiene datos simples, por lo que FreeRTOS puede copiarla de forma
 * segura al almacenamiento interno de la cola.
 */
struct TelemetryData {
    std::uint32_t sequence;
    float temperature_c;
    float humidity_percent;
    std::uint64_t elapsed_time_ms;
};
