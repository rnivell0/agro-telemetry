#pragma once

#include "TelemetryData.hpp"

#include <chrono>

struct PersistableTelemetry
{
    TelemetryData telemetry;
    std::chrono::system_clock::time_point receivedAt;
};
