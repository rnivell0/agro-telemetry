#include "LineProtocolSerializer.hpp"

#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <system_error>

std::string LineProtocolSerializer::serialize(const PersistableTelemetry &value) const
{
    const auto timestampMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 value.receivedAt.time_since_epoch())
                                 .count();

    std::string line;
    line.reserve(160 + value.telemetry.deviceId.size());
    line += "device_telemetry,device_id=";
    line += escapeTagValue(value.telemetry.deviceId);
    line += " temperature_c=";
    line += formatFloat(value.telemetry.temperatureC);
    line += ",humidity_pct=";
    line += formatFloat(value.telemetry.humidityPct);
    line += ",sequence=";
    line += std::to_string(value.telemetry.sequence);
    line += "u,uptime_ms=";
    line += std::to_string(value.telemetry.uptimeMs);
    line += "u ";
    line += std::to_string(timestampMs);
    return line;
}

std::string LineProtocolSerializer::escapeTagValue(const std::string &value)
{
    std::string escaped;
    escaped.reserve(value.size());

    for (const char character : value)
    {
        if (character == '\r' || character == '\n')
        {
            throw std::invalid_argument("line protocol tag values cannot contain CR or LF");
        }
        if (character == ',' || character == ' ' || character == '=' || character == '\\')
        {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }

    return escaped;
}

std::string LineProtocolSerializer::formatFloat(const double value)
{
    if (!std::isfinite(value))
    {
        throw std::invalid_argument("line protocol floating-point fields must be finite");
    }

    char buffer[64]{};
    const auto result = std::to_chars(
        std::begin(buffer),
        std::end(buffer),
        value,
        std::chars_format::general,
        std::numeric_limits<double>::max_digits10);
    if (result.ec != std::errc{})
    {
        throw std::runtime_error("failed to format floating-point field");
    }

    std::string formatted(buffer, result.ptr);
    if (formatted.find_first_of(".eE") == std::string::npos)
    {
        formatted += ".0";
    }
    return formatted;
}
