#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct TelemetryData
{
    std::string deviceId;
    std::uint64_t sequence;
    double temperatureC;
    double humidityPct;
    std::uint64_t uptimeMs;
    // Opcional: un nodo con firmware anterior no lo manda.
    std::optional<int> rssiDbm;
};
