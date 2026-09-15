#pragma once

#include <atomic>
#include <cstdint>

#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"

class WifiManager {
public:
    WifiManager() = default;
    ~WifiManager();

    WifiManager(const WifiManager &) = delete;
    WifiManager &operator=(const WifiManager &) = delete;

    esp_err_t start();
    bool is_connected() const;

private:
    static void event_handler(void *context,
                              esp_event_base_t event_base,
                              std::int32_t event_id,
                              void *event_data);

    void handle_wifi_event(std::int32_t event_id, void *event_data);
    void handle_ip_event(std::int32_t event_id, void *event_data);
    void connect();

    std::atomic<bool> connected_{false};
    esp_netif_t *station_netif_{nullptr};
    esp_event_handler_instance_t wifi_event_instance_{nullptr};
    esp_event_handler_instance_t ip_event_instance_{nullptr};
    bool wifi_initialized_{false};
    bool wifi_started_{false};
};
