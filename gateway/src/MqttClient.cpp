#include "MqttClient.hpp"

#include <chrono>
#include <iostream>
#include <utility>

#include <mqtt/ssl_options.h>

using namespace std::chrono_literals;

MqttClient::MqttClient(MqttConfig config, MessageHandler messageHandler)
    : config_{std::move(config)},
      messageHandler_{std::move(messageHandler)},
      client_{config_.brokerUri, config_.clientId},
      connectOptions_{
          mqtt::connect_options_builder()
              .clean_session(true)
              .keep_alive_interval(20s)
              .user_name(config_.username)
              .password(config_.password)
              .ssl(mqtt::ssl_options_builder()
                       .trust_store(config_.caFile)
                       .finalize())
              .finalize()}
{
    client_.set_callback(*this);
}

MqttClient::~MqttClient()
{
    stop();
}

void MqttClient::start()
{
    if (running_.exchange(true))
    {
        return;
    }

    connectionThread_ = std::thread{&MqttClient::connectionLoop, this};
}

void MqttClient::stop() noexcept
{
    if (!running_.exchange(false))
    {
        return;
    }

    waitCondition_.notify_all();

    if (connectionThread_.joinable())
    {
        connectionThread_.join();
    }

    if (client_.is_connected())
    {
        try
        {
            client_.disconnect()->wait();
            log("INFO", "Disconnected cleanly from MQTT broker");
        }
        catch (const mqtt::exception &error)
        {
            log(
                "WARN",
                std::string{"Error during MQTT shutdown: "} + error.what());
        }
    }
}

void MqttClient::connection_lost(const std::string &cause)
{
    subscribed_.store(false);

    std::string message{"MQTT disconnected; reconnecting"};
    if (!cause.empty())
    {
        message += " cause=" + cause;
    }
    log("WARN", message);
    waitCondition_.notify_all();
}

void MqttClient::message_arrived(mqtt::const_message_ptr message)
{
    try
    {
        messageHandler_(message->get_topic(), message->to_string());
    }
    catch (const std::exception &error)
    {
        log(
            "WARN",
            std::string{"Message handler rejected an event: "} + error.what());
    }
    catch (...)
    {
        log("WARN", "Message handler rejected an event: unknown error");
    }
}

void MqttClient::delivery_complete(mqtt::delivery_token_ptr)
{
}

void MqttClient::connectionLoop() noexcept
{
    while (running_.load())
    {
        try
        {
            if (!client_.is_connected())
            {
                client_.connect(connectOptions_)->wait();
                log("INFO", "Connected to MQTT broker");
                subscribed_.store(false);
            }

            if (!subscribed_.load())
            {
                client_.subscribe(config_.topic, config_.qos)->wait();
                subscribed_.store(true);
                log(
                    "INFO",
                    "Subscribed topic=" + config_.topic +
                        " qos=" + std::to_string(config_.qos));
            }
        }
        catch (const mqtt::exception &error)
        {
            subscribed_.store(false);
            log(
                "WARN",
                std::string{"MQTT unavailable; retrying: "} + error.what());
        }
        catch (const std::exception &error)
        {
            subscribed_.store(false);
            log(
                "WARN",
                std::string{"MQTT connection error; retrying: "} +
                    error.what());
        }
        catch (...)
        {
            subscribed_.store(false);
            log("WARN", "Unknown MQTT connection error; retrying");
        }

        std::unique_lock<std::mutex> lock{waitMutex_};
        waitCondition_.wait_for(
            lock,
            2s,
            [this] { return !running_.load(); });
    }
}

void MqttClient::log(
    const char *level,
    const std::string &message) const
{
    const std::lock_guard<std::mutex> lock{logMutex_};
    std::cout << '[' << level << "] " << message << std::endl;
}
