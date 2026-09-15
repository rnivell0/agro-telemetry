#pragma once

#include <atomic>
#include <cstdint>

#include "esp_err.h"
#include "esp_event.h"
#include "mqtt_client.h"

#include "telemetry_data.hpp"

class WifiManager;

class MqttClient {
public:
    explicit MqttClient(const WifiManager &wifi_manager);
    ~MqttClient();

    MqttClient(const MqttClient &) = delete;
    MqttClient &operator=(const MqttClient &) = delete;

    esp_err_t start();
    esp_err_t publish_telemetry(const TelemetryData &measurement);
    bool is_connected() const;

private:
    static void mqtt_event_handler(void *context,
                                   esp_event_base_t event_base,
                                   std::int32_t event_id,
                                   void *event_data);
    static void network_event_handler(void *context,
                                      esp_event_base_t event_base,
                                      std::int32_t event_id,
                                      void *event_data);

    void handle_mqtt_event(std::int32_t event_id, void *event_data);
    void handle_network_event(esp_event_base_t event_base,
                              std::int32_t event_id);
    esp_err_t start_transport();

    const WifiManager &wifi_manager_;
    esp_mqtt_client_handle_t client_{nullptr};
    esp_event_handler_instance_t got_ip_event_instance_{nullptr};
    esp_event_handler_instance_t wifi_disconnected_event_instance_{nullptr};
    std::atomic<bool> connected_{false};
    bool transport_started_{false};
};
