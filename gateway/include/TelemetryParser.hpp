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

// Devuelve el ID de un topic agro/devices/<id>/telemetry, o nada si el
// topic no tiene exactamente esa forma. La ACL del broker solo deja a cada
// cliente publicar en el topic con su propio client ID, así que este es el
// ID en el que se puede confiar, no el que viene en el payload.
std::optional<std::string> deviceIdFromTopic(const std::string &topic);
