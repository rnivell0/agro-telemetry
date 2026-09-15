#pragma once

#include <cstdint>
#include <string>

struct TelemetryData
{
    std::string deviceId;
    std::uint64_t sequence;
    double temperatureC;
    double humidityPct;
    std::uint64_t uptimeMs;
};
