#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>

#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "sdkconfig.h"

#include "device_id.hpp"
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
    MqttClient(const WifiManager &wifi_manager, const DeviceId &device_id);
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
#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
        ShadowSubscribed,
        ShadowState,
#endif
    };

    // Datos simples, para que FreeRTOS pueda copiarlo a la cola por valor.
    struct Message {
        MessageType type;
        TelemetryData measurement;
        // ShadowState: el intervalo pedido, ya validado, o 0 si el
        // documento no pide ninguno.
        std::uint32_t telemetry_interval_s;
    };
    static_assert(std::is_trivially_copyable_v<Message>,
                  "La cola de FreeRTOS copia los mensajes byte a byte");

    static constexpr UBaseType_t QUEUE_CAPACITY = 10;
#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
    // Hasta que el shadow pida otra cosa, cada medición, como con el
    // broker local.
    static constexpr std::uint32_t DEFAULT_TELEMETRY_INTERVAL_S = 2;
#endif

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
#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
    esp_err_t load_device_identity();
    void subscribe_shadow();
    void request_shadow();
    void apply_telemetry_interval(std::uint32_t requested_interval_s);
    void report_shadow();
    // Corre en la tarea de ESP-MQTT: solo interpreta y pasa el resultado.
    void handle_shadow_data(const esp_mqtt_event_t &event);
    bool is_shadow_topic(const esp_mqtt_event_t &event,
                         const char *suffix) const;
#endif
    void handle_message(const Message &message);
    void start_transport();
    void send_telemetry(const TelemetryData &measurement);
    // Desde las tareas de eventos: nunca bloquea, avisa si se pierde algo.
    void post_event(MessageType type, std::uint32_t telemetry_interval_s = 0);

    const WifiManager &wifi_manager_;
    // Copias propias: ESP-MQTT y cada publicación los usan mientras viva
    // el cliente.
    DeviceId device_id_;
    std::array<char, 48> telemetry_topic_{};
#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
    // Leídos de la partición certs. ESP-MQTT guarda los punteros y los
    // vuelve a usar en cada reconexión, así que viven tanto como el cliente.
    // Un certificado RSA 2048 en PEM ronda los 1,2 KB y su clave los 1,7 KB.
    std::array<char, 2048> device_certificate_{};
    std::array<char, 2048> device_key_{};
    // "$aws/things/<device ID>/shadow/": todos los topics del shadow
    // cuelgan de acá.
    std::array<char, 48> shadow_prefix_{};
#endif
    TaskHandle_t task_handle_{nullptr};
    QueueHandle_t queue_{nullptr};

    // Solo los toca la tarea MQTT.
    esp_mqtt_client_handle_t client_{nullptr};
    esp_event_handler_instance_t got_ip_event_instance_{nullptr};
    esp_event_handler_instance_t wifi_disconnected_event_instance_{nullptr};
    bool transport_started_{false};
#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
    // Cada cuánto se manda una medición a AWS. Lo fija el Device Shadow;
    // el sensor sigue leyendo cada 2 s y la OLED muestra todas.
    std::uint32_t telemetry_interval_s_{DEFAULT_TELEMETRY_INTERVAL_S};
    // uptime de la última medición enviada; 0 si todavía no se envió
    // ninguna.
    std::uint64_t last_sent_ms_{0};
#endif

    // Lo escribe solo la tarea MQTT, y solo según los eventos del broker;
    // las demás lo leen (OLED, publicar).
    std::atomic<bool> connected_{false};
};
