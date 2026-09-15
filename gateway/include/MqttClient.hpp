#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <mqtt/async_client.h>

struct MqttConfig
{
    std::string brokerUri;
    std::string clientId;
    std::string topic;
    int qos;
    std::string caFile;
    std::string username;
    std::string password;
};

class MqttClient final : private virtual mqtt::callback
{
public:
    using MessageHandler =
        std::function<void(const std::string &, const std::string &)>;

    MqttClient(MqttConfig config, MessageHandler messageHandler);
    ~MqttClient() override;

    MqttClient(const MqttClient &) = delete;
    MqttClient &operator=(const MqttClient &) = delete;

    void start();
    void stop() noexcept;

private:
    void connection_lost(const std::string &cause) override;
    void message_arrived(mqtt::const_message_ptr message) override;
    void delivery_complete(mqtt::delivery_token_ptr token) override;

    void connectionLoop() noexcept;
    void log(const char *level, const std::string &message) const;

    MqttConfig config_;
    MessageHandler messageHandler_;
    mqtt::async_client client_;
    mqtt::connect_options connectOptions_;

    std::atomic<bool> running_{false};
    std::atomic<bool> subscribed_{false};
    std::thread connectionThread_;
    std::mutex waitMutex_;
    std::condition_variable waitCondition_;
    mutable std::mutex logMutex_;
};
