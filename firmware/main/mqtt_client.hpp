#pragma once

#include <atomic>
#include <cstdint>
#include <type_traits>

#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "telemetry_data.hpp"

class WifiManager;

/**
 * Cliente MQTT con una tarea propia que es la única dueña del cliente de
 * ESP-MQTT y de su estado.
 *
 * Los eventos de red (tarea de eventos de ESP-IDF), los del cliente MQTT
 * (tarea interna de ESP-MQTT) y las publicaciones (TelemetryTask) no tocan
 * el estado directamente: se convierten en mensajes de una cola, y esta
 * tarea los procesa de a uno y en orden. Así ninguna decisión, como
 * arrancar el transporte, puede tomarse desde dos tareas a la vez.
 */
class MqttClient {
public:
    explicit MqttClient(const WifiManager &wifi_manager);
    ~MqttClient();

    MqttClient(const MqttClient &) = delete;
    MqttClient &operator=(const MqttClient &) = delete;

    esp_err_t start();

    /**
     * Encola una medición para que la tarea MQTT la publique. Nunca
     * bloquea: devuelve ESP_ERR_INVALID_STATE si no hay conexión con el
     * broker y ESP_ERR_NO_MEM si la cola de la tarea está llena.
     */
    esp_err_t publish_telemetry(const TelemetryData &measurement);
    bool is_connected() const;

private:
    // Lo que puede pedirle cualquier otra tarea a la tarea MQTT.
    enum class MessageType : std::uint8_t {
        NetworkUp,
        NetworkDown,
        BrokerConnected,
        BrokerDisconnected,
        Publish,
    };

    // Datos simples, para que FreeRTOS pueda copiarlo a la cola por valor.
    struct Message {
        MessageType type;
        TelemetryData measurement;
    };
    static_assert(std::is_trivially_copyable_v<Message>,
                  "La cola de FreeRTOS copia los mensajes byte a byte");

    static constexpr UBaseType_t QUEUE_CAPACITY = 10;

    static void task_entry(void *context);
    static void mqtt_event_handler(void *context,
                                   esp_event_base_t event_base,
                                   std::int32_t event_id,
                                   void *event_data);
    static void network_event_handler(void *context,
                                      esp_event_base_t event_base,
                                      std::int32_t event_id,
                                      void *event_data);

    void run();
    esp_err_t initialize();
    void handle_message(const Message &message);
    void start_transport();
    void send_telemetry(const TelemetryData &measurement);
    // Desde las tareas de eventos: nunca bloquea, avisa si se pierde algo.
    void post_event(MessageType type);

    const WifiManager &wifi_manager_;
    TaskHandle_t task_handle_{nullptr};
    QueueHandle_t queue_{nullptr};

    // Solo los toca la tarea MQTT.
    esp_mqtt_client_handle_t client_{nullptr};
    esp_event_handler_instance_t got_ip_event_instance_{nullptr};
    esp_event_handler_instance_t wifi_disconnected_event_instance_{nullptr};
    bool transport_started_{false};

    // Lo escribe solo la tarea MQTT; las demás lo leen (OLED, publicar).
    std::atomic<bool> connected_{false};
};
