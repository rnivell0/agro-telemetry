#include "TelemetryParser.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace
{
int failures = 0;

void expect(bool condition, const std::string &description)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << description << '\n';
        ++failures;
    }
}
}

int main()
{
    const TelemetryParser parser;

    const auto valid = parser.parse(
        R"({"device_id":"esp32-node-01","sequence":930,"temperature_c":22.0,"humidity_pct":66.0,"uptime_ms":930290})");
    expect(static_cast<bool>(valid), "valid message must be accepted");
    if (valid)
    {
        expect(valid.data->deviceId == "esp32-node-01", "device_id parsed");
        expect(valid.data->sequence == 930, "sequence parsed");
        expect(valid.data->temperatureC == 22.0, "temperature parsed");
        expect(valid.data->humidityPct == 66.0, "humidity parsed");
        expect(valid.data->uptimeMs == 930290, "uptime parsed");
    }

    const auto malformed = parser.parse("{not-json");
    expect(!malformed, "malformed JSON must be rejected");

    const auto missing = parser.parse(
        R"({"device_id":"node","sequence":1,"temperature_c":20,"uptime_ms":100})");
    expect(!missing, "missing humidity must be rejected");
    expect(
        missing.error == "missing field humidity_pct",
        "missing field error must be descriptive");

    const auto invalidHumidity = parser.parse(
        R"({"device_id":"node","sequence":1,"temperature_c":20,"humidity_pct":101,"uptime_ms":100})");
    expect(!invalidHumidity, "humidity over 100 must be rejected");

    const auto negativeSequence = parser.parse(
        R"({"device_id":"node","sequence":-1,"temperature_c":20,"humidity_pct":50,"uptime_ms":100})");
    expect(!negativeSequence, "negative sequence must be rejected");

    const auto emptyDevice = parser.parse(
        R"({"device_id":"","sequence":1,"temperature_c":20,"humidity_pct":50,"uptime_ms":100})");
    expect(!emptyDevice, "empty device must be rejected");

    const auto deviceWithLineFeed = parser.parse(
        "{\"device_id\":\"node\\ninjected\",\"sequence\":1,"
        "\"temperature_c\":20,\"humidity_pct\":50,\"uptime_ms\":100}");
    expect(
        !deviceWithLineFeed,
        "device_id with a line feed must be rejected");

    if (failures == 0)
    {
        std::cout << "All telemetry parser tests passed\n";
        return EXIT_SUCCESS;
    }

    return EXIT_FAILURE;
}
