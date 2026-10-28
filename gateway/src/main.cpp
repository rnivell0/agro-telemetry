#include "InfluxWriter.hpp"
#include "MqttClient.hpp"
#include "PersistableTelemetry.hpp"
#include "PersistenceQueue.hpp"
#include "TelemetryParser.hpp"

#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>

namespace
{
volatile std::sig_atomic_t stopRequested = 0;
std::mutex logMutex;

void requestStop(int)
{
    stopRequested = 1;
}

std::string requireEnvironment(const char *name)
{
    const char *value = std::getenv(name);
    if (value == nullptr || *value == '\0')
    {
        throw std::runtime_error{
            std::string{"missing environment variable "} + name};
    }
    return value;
}

std::string readFileTrimmed(const std::string &path)
{
    std::ifstream input{path};
    if (!input)
    {
        throw std::runtime_error{"unable to read secret file " + path};
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string value = buffer.str();

    while (!value.empty() &&
           std::isspace(static_cast<unsigned char>(value.back())) != 0)
    {
        value.pop_back();
    }

    if (value.empty())
    {
        throw std::runtime_error{"secret file is empty: " + path};
    }
    return value;
}

int parseQos(const std::string &text)
{
    int qos = -1;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), qos);

    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() ||
        qos < 0 ||
        qos > 2)
    {
        throw std::runtime_error{"MQTT_QOS must be 0, 1 or 2"};
    }
    return qos;
}

std::uint64_t parsePositiveInteger(
    const char *name,
    const std::string &text)
{
    std::uint64_t value = 0;
    const auto result =
        std::from_chars(text.data(), text.data() + text.size(), value);

    if (result.ec != std::errc{} ||
        result.ptr != text.data() + text.size() ||
        value == 0)
    {
        throw std::runtime_error{
            std::string{name} + " must be a positive integer"};
    }
    return value;
}

std::size_t parseSize(
    const char *name,
    const std::string &text)
{
    const std::uint64_t value = parsePositiveInteger(name, text);
    if (value > std::numeric_limits<std::size_t>::max())
    {
        throw std::runtime_error{
            std::string{name} + " is too large"};
    }
    return static_cast<std::size_t>(value);
}

template <typename Duration>
Duration parseDuration(
    const char *name,
    const std::string &text)
{
    const std::uint64_t value = parsePositiveInteger(name, text);
    using Representation = typename Duration::rep;
    if (value >
        static_cast<std::uint64_t>(
            std::numeric_limits<Representation>::max()))
    {
        throw std::runtime_error{
            std::string{name} + " is too large"};
    }
    return Duration{static_cast<Representation>(value)};
}

void log(const char *level, const std::string &message)
{
    const std::lock_guard<std::mutex> lock{logMutex};
    std::cout << '[' << level << "] " << message << std::endl;
}
}

int main()
{
    try
    {
        const MqttConfig config{
            requireEnvironment("MQTT_BROKER_URI"),
            requireEnvironment("MQTT_CLIENT_ID"),
            requireEnvironment("MQTT_TOPIC"),
            parseQos(requireEnvironment("MQTT_QOS")),
            requireEnvironment("MQTT_CA_FILE"),
            requireEnvironment("MQTT_USERNAME"),
            readFileTrimmed(requireEnvironment("MQTT_PASSWORD_FILE")),
        };
        const std::size_t queueCapacity = parseSize(
            "INFLUX_QUEUE_CAPACITY",
            requireEnvironment("INFLUX_QUEUE_CAPACITY"));
        const InfluxConfig influxConfig{
            requireEnvironment("INFLUX_URL"),
            requireEnvironment("INFLUX_ORG"),
            requireEnvironment("INFLUX_BUCKET"),
            requireEnvironment("INFLUX_TOKEN_FILE"),
            parseSize(
                "INFLUX_BATCH_SIZE",
                requireEnvironment("INFLUX_BATCH_SIZE")),
            parseDuration<std::chrono::milliseconds>(
                "INFLUX_FLUSH_INTERVAL_MS",
                requireEnvironment("INFLUX_FLUSH_INTERVAL_MS")),
            parseDuration<std::chrono::seconds>(
                "INFLUX_RETRY_MAX_SECONDS",
                requireEnvironment("INFLUX_RETRY_MAX_SECONDS")),
        };

        std::signal(SIGINT, requestStop);
        std::signal(SIGTERM, requestStop);

        TelemetryParser parser;
        PersistenceQueue persistenceQueue{queueCapacity};
        InfluxWriter influxWriter{influxConfig, persistenceQueue};
        std::atomic<std::uint64_t> validMessages{0};
        std::atomic<std::uint64_t> invalidMessages{0};
        std::unordered_map<std::string, std::uint64_t> lastSequence;
        std::mutex sequenceMutex;

        MqttClient mqttClient{
            config,
            [&](const std::string &topic, const std::string &payload)
            {
                const auto receivedAt =
                    std::chrono::system_clock::now();

                // El topic no se loguea: puede traer cualquier carácter.
                const std::optional<std::string> topicDeviceId =
                    deviceIdFromTopic(topic);
                if (!topicDeviceId)
                {
                    ++invalidMessages;
                    log("WARN", "Invalid telemetry: unexpected topic");
                    return;
                }

                TelemetryParseResult result = parser.parse(payload);
                if (!result)
                {
                    ++invalidMessages;
                    log("WARN", "Invalid telemetry: " + result.error);
                    return;
                }

                TelemetryData data = std::move(*result.data);

                /*
                 * La ACL garantiza el ID del topic, no el del payload. Si no
                 * coinciden, el mensaje viene de un nodo mal configurado o
                 * de alguien que intenta escribir a nombre de otro nodo.
                 */
                if (data.deviceId != *topicDeviceId)
                {
                    ++invalidMessages;
                    log("WARN",
                        "Invalid telemetry: device_id " + data.deviceId +
                            " does not match topic device " +
                            *topicDeviceId);
                    return;
                }

                {
                    const std::lock_guard<std::mutex> lock{sequenceMutex};
                    const auto previous = lastSequence.find(data.deviceId);
                    if (previous != lastSequence.end())
                    {
                        const std::uint64_t expected =
                            previous->second + 1;

                        if (data.sequence > expected)
                        {
                            log(
                                "WARN",
                                "device=" + data.deviceId +
                                    " sequence gap: expected=" +
                                    std::to_string(expected) +
                                    " received=" +
                                    std::to_string(data.sequence));
                        }
                        else if (data.sequence <= previous->second)
                        {
                            log(
                                "WARN",
                                "device=" + data.deviceId +
                                    " sequence rollback: previous=" +
                                    std::to_string(previous->second) +
                                    " received=" +
                                    std::to_string(data.sequence));
                        }
                    }
                    lastSequence[data.deviceId] = data.sequence;
                }

                ++validMessages;

                std::ostringstream message;
                message << std::fixed << std::setprecision(1)
                        << "device=" << data.deviceId
                        << " sequence=" << data.sequence
                        << " temperature_c=" << data.temperatureC
                        << " humidity_pct=" << data.humidityPct
                        << " uptime_ms=" << data.uptimeMs;
                log("INFO", message.str());

                const QueuePushResult pushResult =
                    persistenceQueue.push(PersistableTelemetry{
                        std::move(data),
                        receivedAt,
                    });

                if (pushResult == QueuePushResult::DroppedOldest)
                {
                    log(
                        "WARN",
                        "Persistence queue full dropped_oldest=" +
                            std::to_string(
                                persistenceQueue.droppedCount()));
                }
                else if (
                    pushResult == QueuePushResult::RejectedStopped)
                {
                    log(
                        "WARN",
                        "Persistence queue stopped; "
                        "measurement not queued");
                }
            }};

        influxWriter.start();
        mqttClient.start();

        while (stopRequested == 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds{200});
        }

        mqttClient.stop();
        influxWriter.stop();
        const InfluxWriterStats influxStats = influxWriter.stats();
        log(
            "INFO",
            "Gateway stopped valid_messages=" +
                std::to_string(validMessages.load()) +
                " invalid_messages=" +
                std::to_string(invalidMessages.load()) +
                " persistence_queue_dropped=" +
                std::to_string(persistenceQueue.droppedCount()) +
                " influx_points_written=" +
                std::to_string(influxStats.pointsWritten) +
                " influx_points_discarded=" +
                std::to_string(influxStats.pointsDiscarded) +
                " influx_retry_attempts=" +
                std::to_string(influxStats.retryAttempts));
        return EXIT_SUCCESS;
    }
    catch (const std::exception &error)
    {
        log("ERROR", error.what());
        return EXIT_FAILURE;
    }
}
