#pragma once

#include <optional>
#include <string>

#include "TelemetryData.hpp"

struct TelemetryParseResult
{
    std::optional<TelemetryData> data;
    std::string error;

    explicit operator bool() const noexcept
    {
        return data.has_value();
    }
};

class TelemetryParser
{
public:
    TelemetryParseResult parse(const std::string &payload) const noexcept;
};
