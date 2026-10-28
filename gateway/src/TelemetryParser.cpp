#include "TelemetryParser.hpp"

#include <cmath>
#include <exception>
#include <string_view>

#include <nlohmann/json.hpp>

namespace
{
using Json = nlohmann::json;

std::string missingField(const char *field)
{
    return std::string{"missing field "} + field;
}

std::string invalidType(const char *field)
{
    return std::string{"invalid type for field "} + field;
}
}

std::optional<std::string> deviceIdFromTopic(const std::string &topic)
{
    constexpr std::string_view prefix{"agro/devices/"};
    constexpr std::string_view suffix{"/telemetry"};

    const std::string_view view{topic};
    if (view.size() <= prefix.size() + suffix.size() ||
        view.substr(0, prefix.size()) != prefix ||
        view.substr(view.size() - suffix.size()) != suffix)
    {
        return std::nullopt;
    }

    const std::string_view deviceId = view.substr(
        prefix.size(), view.size() - prefix.size() - suffix.size());

    // Un solo nivel: con '/' el topic tendría niveles de más.
    if (deviceId.find_first_of("/+#\r\n") != std::string_view::npos)
    {
        return std::nullopt;
    }

    return std::string{deviceId};
}

TelemetryParseResult TelemetryParser::parse(
    const std::string &payload) const noexcept
{
    try
    {
        const Json document = Json::parse(payload);

        if (!document.is_object())
        {
            return {std::nullopt, "root value must be an object"};
        }

        constexpr const char *requiredFields[] = {
            "device_id",
            "sequence",
            "temperature_c",
            "humidity_pct",
            "uptime_ms",
        };

        for (const char *field : requiredFields)
        {
            if (!document.contains(field))
            {
                return {std::nullopt, missingField(field)};
            }
        }

        if (!document["device_id"].is_string())
        {
            return {std::nullopt, invalidType("device_id")};
        }
        if (!document["sequence"].is_number_unsigned())
        {
            return {std::nullopt, invalidType("sequence")};
        }
        if (!document["temperature_c"].is_number())
        {
            return {std::nullopt, invalidType("temperature_c")};
        }
        if (!document["humidity_pct"].is_number())
        {
            return {std::nullopt, invalidType("humidity_pct")};
        }
        if (!document["uptime_ms"].is_number_unsigned())
        {
            return {std::nullopt, invalidType("uptime_ms")};
        }

        TelemetryData telemetry{
            document["device_id"].get<std::string>(),
            document["sequence"].get<std::uint64_t>(),
            document["temperature_c"].get<double>(),
            document["humidity_pct"].get<double>(),
            document["uptime_ms"].get<std::uint64_t>(),
        };

        if (telemetry.deviceId.empty())
        {
            return {std::nullopt, "field device_id must not be empty"};
        }
        if (telemetry.deviceId.find_first_of("\r\n") != std::string::npos)
        {
            return {
                std::nullopt,
                "field device_id must not contain CR or LF",
            };
        }
        if (!std::isfinite(telemetry.temperatureC))
        {
            return {std::nullopt, "field temperature_c must be finite"};
        }
        if (!std::isfinite(telemetry.humidityPct))
        {
            return {std::nullopt, "field humidity_pct must be finite"};
        }
        if (telemetry.humidityPct < 0.0 || telemetry.humidityPct > 100.0)
        {
            return {
                std::nullopt,
                "field humidity_pct must be between 0 and 100",
            };
        }

        return {std::move(telemetry), {}};
    }
    catch (const Json::parse_error &error)
    {
        return {
            std::nullopt,
            std::string{"malformed JSON: "} + error.what(),
        };
    }
    catch (const std::exception &error)
    {
        return {
            std::nullopt,
            std::string{"invalid telemetry: "} + error.what(),
        };
    }
    catch (...)
    {
        return {std::nullopt, "unknown telemetry parsing error"};
    }
}
