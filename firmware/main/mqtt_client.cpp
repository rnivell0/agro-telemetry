#include "mqtt_client.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>

#include "esp_log.h"
#include "esp_wifi.h"
#include "sdkconfig.h"

#include "wifi_manager.hpp"

namespace {
constexpr char TAG[] = "MQTT_CLIENT";
constexpr int JSON_BUFFER_SIZE = 192;
constexpr int MQTT_QOS = 1;
constexpr bool MQTT_RETAIN = false;

/*
 * Certificado de la CA local incrustado en el binario mediante
 * EMBED_TXTFILES (ver main/CMakeLists.txt). ESP-MQTT lo usa para validar
 * el certificado que presenta el broker durante el handshake TLS.
 */
extern const std::uint8_t mqtt_ca_pem_start[] asm(
    "_binary_mqtt_ca_pem_start");
extern const std::uint8_t mqtt_ca_pem_end[] asm("_binary_mqtt_ca_pem_end");
}

MqttClient::MqttClient(const WifiManager &wifi_manager)
    : wifi_manager_{wifi_manager}
{
}

MqttClient::~MqttClient()
{
    connected_.store(false);

    if (got_ip_event_instance_ != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_event_handler_instance_unregister(
                IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip_event_instance_));
    }

    if (wifi_disconnected_event_instance_ != nullptr) {
        ESP_ERROR_CHECK_WITHOUT_ABORT(
            esp_event_handler_instance_unregister(
                WIFI_EVENT,
                WIFI_EVENT_STA_DISCONNECTED,
                wifi_disconnected_event_instance_));
    }

    if (client_ != nullptr) {
        if (transport_started_) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_mqtt_client_stop(client_));
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_mqtt_client_destroy(client_));
    }
}

esp_err_t MqttClient::start()
{
    if (client_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_mqtt_client_config_t configuration{};
    configuration.broker.address.uri = CONFIG_AGRO_MQTT_BROKER_URI;
    configuration.broker.verification.certificate =
        reinterpret_cast<const char *>(mqtt_ca_pem_start);
    configuration.credentials.client_id = CONFIG_AGRO_MQTT_CLIENT_ID;
    configuration.credentials.username = CONFIG_AGRO_MQTT_USERNAME;
    configuration.credentials.authentication.password =
        CONFIG_AGRO_MQTT_PASSWORD;
    configuration.network.disable_auto_reconnect = false;

    client_ = esp_mqtt_client_init(&configuration);
    if (client_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t result = esp_mqtt_client_register_event(
        client_, MQTT_EVENT_ANY, &MqttClient::mqtt_event_handler, this);
    if (result != ESP_OK) {
        return result;
    }

    result = esp_event_handler_instance_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &MqttClient::network_event_handler,
        this,
        &got_ip_event_instance_);
    if (result != ESP_OK) {
        return result;
    }

    result = esp_event_handler_instance_register(
        WIFI_EVENT,
        WIFI_EVENT_STA_DISCONNECTED,
        &MqttClient::network_event_handler,
        this,
        &wifi_disconnected_event_instance_);
    if (result != ESP_OK) {
        return result;
    }

    ESP_LOGI(TAG,
             "Cliente preparado | Broker: %s | Client ID: %s | Usuario: %s",
             CONFIG_AGRO_MQTT_BROKER_URI,
             CONFIG_AGRO_MQTT_CLIENT_ID,
             CONFIG_AGRO_MQTT_USERNAME);

    /*
     * Esta comprobacion unica cubre el caso en que Wi-Fi obtuvo IP antes de
     * registrar los eventos. No se repite y por lo tanto no es polling.
     */
    if (wifi_manager_.is_connected()) {
        return start_transport();
    }

    ESP_LOGI(TAG, "Esperando direccion IP para iniciar MQTT");
    return ESP_OK;
}

esp_err_t MqttClient::publish_telemetry(const TelemetryData &measurement)
{
    if (!connected_.load() || client_ == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    char payload[JSON_BUFFER_SIZE]{};
    const int payload_length = std::snprintf(
        payload,
        sizeof(payload),
        "{\"device_id\":\"%s\",\"sequence\":%" PRIu32
        ",\"temperature_c\":%.1f,\"humidity_pct\":%.1f,"
        "\"uptime_ms\":%" PRIu64 "}",
        CONFIG_AGRO_MQTT_CLIENT_ID,
        measurement.sequence,
        static_cast<double>(measurement.temperature_c),
        static_cast<double>(measurement.humidity_percent),
        measurement.elapsed_time_ms);

    if (payload_length < 0 ||
        static_cast<std::size_t>(payload_length) >= sizeof(payload)) {
        return ESP_ERR_INVALID_SIZE;
    }

    /*
     * enqueue entrega el mensaje al outbox y retorna sin esperar la red.
     * La tarea interna de ESP-MQTT realiza el envio y espera el ACK de QoS 1.
     */
    const int message_id = esp_mqtt_client_enqueue(
        client_,
        CONFIG_AGRO_MQTT_TELEMETRY_TOPIC,
        payload,
        payload_length,
        MQTT_QOS,
        MQTT_RETAIN,
        true);

    if (message_id < 0) {
        return ESP_FAIL;
    }

    ESP_LOGI(TAG,
             "Telemetria encolada | Secuencia: %" PRIu32
             " | Message ID: %d",
             measurement.sequence,
             message_id);
    return ESP_OK;
}

bool MqttClient::is_connected() const
{
    return connected_.load();
}

void MqttClient::mqtt_event_handler(void *context,
                                    esp_event_base_t,
                                    std::int32_t event_id,
                                    void *event_data)
{
    auto *mqtt_client = static_cast<MqttClient *>(context);
    mqtt_client->handle_mqtt_event(event_id, event_data);
}

void MqttClient::network_event_handler(void *context,
                                       esp_event_base_t event_base,
                                       std::int32_t event_id,
                                       void *)
{
    auto *mqtt_client = static_cast<MqttClient *>(context);
    mqtt_client->handle_network_event(event_base, event_id);
}

void MqttClient::handle_mqtt_event(std::int32_t event_id, void *event_data)
{
    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        connected_.store(true);
        ESP_LOGI(TAG, "Conectado al broker MQTT");
        break;

    case MQTT_EVENT_DISCONNECTED:
        connected_.store(false);
        ESP_LOGW(TAG, "Desconectado del broker; ESP-MQTT reintentara");
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG,
                 "Publicacion QoS 1 confirmada | Message ID: %d",
                 event->msg_id);
        break;

    case MQTT_EVENT_ERROR:
        connected_.store(false);
        ESP_LOGE(TAG, "Error reportado por el cliente MQTT");
        break;

    default:
        break;
    }
}

void MqttClient::handle_network_event(esp_event_base_t event_base,
                                      std::int32_t event_id)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (!transport_started_) {
            const esp_err_t result = start_transport();
            if (result != ESP_OK) {
                ESP_LOGE(TAG,
                         "No se pudo iniciar MQTT: %s",
                         esp_err_to_name(result));
            }
        } else {
            ESP_LOGI(TAG,
                     "Wi-Fi recuperado; MQTT continuara su reconexion automatica");
        }
        return;
    }

    if (event_base == WIFI_EVENT &&
        event_id == WIFI_EVENT_STA_DISCONNECTED) {
        connected_.store(false);
        ESP_LOGW(TAG, "Wi-Fi no disponible; MQTT queda a la espera");
    }
}

esp_err_t MqttClient::start_transport()
{
    if (transport_started_) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Direccion IP disponible; conectando al broker");
    const esp_err_t result = esp_mqtt_client_start(client_);
    if (result == ESP_OK) {
        transport_started_ = true;
    }
    return result;
}
