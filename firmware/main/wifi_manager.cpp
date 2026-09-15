#include "wifi_manager.hpp"

#include <cstdint>
#include <cstring>

#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

namespace {
constexpr char TAG[] = "WIFI_MANAGER";
}

WifiManager::~WifiManager()
{
    connected_.store(false);

    if (wifi_event_instance_ != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_event_handler_instance_unregister(
                WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_instance_));
    }

    if (ip_event_instance_ != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_event_handler_instance_unregister(
                IP_EVENT, IP_EVENT_STA_GOT_IP, ip_event_instance_));
    }

    if (wifi_started_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_stop());
    }

    if (wifi_initialized_) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_wifi_deinit());
    }

    if (station_netif_ != nullptr) {
        esp_netif_destroy_default_wifi(station_netif_);
    }
}

esp_err_t WifiManager::start()
{
    if (wifi_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t result = nvs_flash_init();
    if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
        result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        result = nvs_flash_erase();
        if (result != ESP_OK) {
            return result;
        }
        result = nvs_flash_init();
    }
    if (result != ESP_OK) {
        return result;
    }

    result = esp_netif_init();
    if (result != ESP_OK) {
        return result;
    }

    result = esp_event_loop_create_default();
    if (result != ESP_OK) {
        return result;
    }

    station_netif_ = esp_netif_create_default_wifi_sta();
    if (station_netif_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    wifi_init_config_t initialization = WIFI_INIT_CONFIG_DEFAULT();
    result = esp_wifi_init(&initialization);
    if (result != ESP_OK) {
        return result;
    }
    wifi_initialized_ = true;

    result = esp_event_handler_instance_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &WifiManager::event_handler,
        this,
        &wifi_event_instance_);
    if (result != ESP_OK) {
        return result;
    }

    result = esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &WifiManager::event_handler,
        this,
        &ip_event_instance_);
    if (result != ESP_OK) {
        return result;
    }

    wifi_config_t wifi_configuration{};
    std::strncpy(
        reinterpret_cast<char *>(wifi_configuration.sta.ssid),
        CONFIG_AGRO_WIFI_SSID,
        sizeof(wifi_configuration.sta.ssid) - 1);
    std::strncpy(
        reinterpret_cast<char *>(wifi_configuration.sta.password),
        CONFIG_AGRO_WIFI_PASSWORD,
        sizeof(wifi_configuration.sta.password) - 1);

    result = esp_wifi_set_mode(WIFI_MODE_STA);
    if (result != ESP_OK) {
        return result;
    }

    result = esp_wifi_set_config(WIFI_IF_STA, &wifi_configuration);
    if (result != ESP_OK) {
        return result;
    }

    ESP_LOGI(TAG, "Iniciando Wi-Fi en modo estacion; SSID: %s",
             CONFIG_AGRO_WIFI_SSID);

    result = esp_wifi_start();
    if (result != ESP_OK) {
        return result;
    }
    wifi_started_ = true;

    return ESP_OK;
}

bool WifiManager::is_connected() const
{
    return connected_.load();
}

void WifiManager::event_handler(void *context,
                                esp_event_base_t event_base,
                                std::int32_t event_id,
                                void *event_data)
{
    auto *manager = static_cast<WifiManager *>(context);

    if (event_base == WIFI_EVENT) {
        manager->handle_wifi_event(event_id, event_data);
    } else if (event_base == IP_EVENT) {
        manager->handle_ip_event(event_id, event_data);
    }
}

void WifiManager::handle_wifi_event(std::int32_t event_id, void *event_data)
{
    if (event_id == WIFI_EVENT_STA_START) {
        connect();
        return;
    }

    if (event_id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "Conexion establecida con el access point");
        return;
    }

    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        connected_.store(false);

        const auto *disconnected =
            static_cast<wifi_event_sta_disconnected_t *>(event_data);
        ESP_LOGW(TAG,
                 "Wi-Fi desconectado (motivo %u); intentando reconectar",
                 static_cast<unsigned>(disconnected->reason));
        connect();
    }
}

void WifiManager::handle_ip_event(std::int32_t event_id, void *event_data)
{
    if (event_id != IP_EVENT_STA_GOT_IP) {
        return;
    }

    const auto *got_ip = static_cast<ip_event_got_ip_t *>(event_data);
    connected_.store(true);
    ESP_LOGI(TAG, "Direccion IP obtenida: " IPSTR, IP2STR(&got_ip->ip_info.ip));
}

void WifiManager::connect()
{
    ESP_LOGI(TAG, "Iniciando conexion al access point");

    const esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "No se pudo iniciar la conexion Wi-Fi: %s",
                 esp_err_to_name(result));
    }
}
