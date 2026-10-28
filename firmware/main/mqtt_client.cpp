#include "mqtt_client.hpp"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "wifi_manager.hpp"

namespace {
constexpr char TAG[] = "MQTT_CLIENT";
constexpr int JSON_BUFFER_SIZE = 192;
constexpr int MQTT_QOS = 1;
constexpr bool MQTT_RETAIN = false;
constexpr std::uint32_t TASK_STACK_SIZE = 3072;
constexpr UBaseType_t TASK_PRIORITY = 5;
constexpr std::uint32_t QUEUE_WAIT_MS = 2000;

// Ver TelemetryTask: misma regla para no reiniciar la placa en reposo.
static_assert(QUEUE_WAIT_MS < CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000U / 2,
              "QUEUE_WAIT_MS tiene que ser menor a la mitad del timeout "
              "del task watchdog");

/*
 * Certificados incrustados en el binario mediante EMBED_TXTFILES (ver
 * main/CMakeLists.txt), que les agrega el '\0' final que espera ESP-MQTT.
 * La CA valida el certificado que presenta el broker en el handshake TLS.
 */
#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
extern const char aws_root_ca_pem_start[] asm("_binary_aws_root_ca_pem_start");

constexpr char BROKER_URI[] = "mqtts://" CONFIG_AGRO_AWS_IOT_ENDPOINT ":8883";

// Dónde graba tools/aws_identity.py la identidad de cada placa.
constexpr char IDENTITY_PARTITION[] = "certs";
constexpr char IDENTITY_NAMESPACE[] = "aws_iot";
#else
extern const char mqtt_ca_pem_start[] asm("_binary_mqtt_ca_pem_start");
#endif
}

MqttClient::MqttClient(const WifiManager &wifi_manager,
                       const DeviceId &device_id)
    : wifi_manager_{wifi_manager}, device_id_{device_id}
{
    std::snprintf(telemetry_topic_.data(),
                  telemetry_topic_.size(),
                  "agro/devices/%s/telemetry",
                  device_id_.data());
}

MqttClient::~MqttClient()
{
    /*
     * Orden inverso al de start(): primero se cortan las fuentes de
     * mensajes, después la tarea que los consume y por último la cola.
     */
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

    if (task_handle_ != nullptr) {
        // ESP-IDF pide desuscribir la tarea del watchdog antes de borrarla.
        esp_task_wdt_delete(task_handle_);
        vTaskDelete(task_handle_);
    }

    if (client_ != nullptr) {
        if (transport_started_) {
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_mqtt_client_stop(client_));
        }
        ESP_ERROR_CHECK_WITHOUT_ABORT(esp_mqtt_client_destroy(client_));
    }

    if (queue_ != nullptr) {
        vQueueDelete(queue_);
    }
}

esp_err_t MqttClient::start()
{
    if (task_handle_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * La cola va primero: los handlers que registra initialize() pueden
     * dispararse antes de que exista la tarea, y sus mensajes esperan en
     * la cola hasta que la tarea arranque.
     */
    queue_ = xQueueCreate(QUEUE_CAPACITY, sizeof(Message));
    if (queue_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // Todavía no existe la tarea MQTT, así que tocar el estado desde acá
    // no compite con nadie.
    const esp_err_t result = initialize();
    if (result != ESP_OK) {
        return result;
    }

    const BaseType_t created = xTaskCreate(task_entry,
                                           "MqttClient",
                                           TASK_STACK_SIZE,
                                           this,
                                           TASK_PRIORITY,
                                           &task_handle_);
    if (created != pdPASS) {
        return ESP_ERR_NO_MEM;
    }

    /*
     * Cubre el caso en que Wi-Fi obtuvo IP antes de registrar el handler.
     * Antes esta comprobación arrancaba el transporte directamente y podía
     * competir con el handler de IP; ahora solo deja un mensaje más en la
     * cola. Si los dos llegan, la tarea arranca con el primero e ignora el
     * segundo.
     */
    if (wifi_manager_.is_connected()) {
        post_event(MessageType::NetworkUp);
    } else {
        ESP_LOGI(TAG, "Esperando direccion IP para iniciar MQTT");
    }
    return ESP_OK;
}

esp_err_t MqttClient::initialize()
{
    esp_mqtt_client_config_t configuration{};
    // En los dos brokers el client ID es el que autoriza: la ACL de
    // Mosquitto y la política de AWS lo comparan con el topic.
    configuration.credentials.client_id = device_id_.data();
    configuration.network.disable_auto_reconnect = false;

#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
    // AWS IoT Core exige TLS mutuo: el nodo también presenta su
    // certificado, y la política del thing decide qué puede hacer.
    const esp_err_t identity_result = load_device_identity();
    if (identity_result != ESP_OK) {
        return identity_result;
    }
    configuration.broker.address.uri = BROKER_URI;
    configuration.broker.verification.certificate = aws_root_ca_pem_start;
    configuration.credentials.authentication.certificate =
        device_certificate_.data();
    configuration.credentials.authentication.key = device_key_.data();
    const char *const broker = BROKER_URI;
    const char *const authentication = "certificado X.509";
#else
    configuration.broker.address.uri = CONFIG_AGRO_MQTT_BROKER_URI;
    configuration.broker.verification.certificate = mqtt_ca_pem_start;
    configuration.credentials.username = CONFIG_AGRO_MQTT_USERNAME;
    configuration.credentials.authentication.password =
        CONFIG_AGRO_MQTT_PASSWORD;
    const char *const broker = CONFIG_AGRO_MQTT_BROKER_URI;
    const char *const authentication = "usuario " CONFIG_AGRO_MQTT_USERNAME;
#endif

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
             "Cliente preparado | Broker: %s | Client ID: %s | Autenticacion: %s"
             " | Topic: %s",
             broker,
             device_id_.data(),
             authentication,
             telemetry_topic_.data());
    return ESP_OK;
}

#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
esp_err_t MqttClient::load_device_identity()
{
    esp_err_t result = nvs_flash_init_partition(IDENTITY_PARTITION);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "No se pudo abrir la particion %s: %s",
                 IDENTITY_PARTITION,
                 esp_err_to_name(result));
        return result;
    }

    nvs_handle_t handle{};
    result = nvs_open_from_partition(
        IDENTITY_PARTITION, IDENTITY_NAMESPACE, NVS_READONLY, &handle);
    if (result == ESP_OK) {
        std::size_t length = device_certificate_.size();
        result = nvs_get_str(
            handle, "device_crt", device_certificate_.data(), &length);
        if (result == ESP_OK) {
            length = device_key_.size();
            result = nvs_get_str(handle, "device_key", device_key_.data(), &length);
        }
        nvs_close(handle);
    }

    // Ya copiados a los buffers propios: la partición no hace falta abierta.
    nvs_flash_deinit_partition(IDENTITY_PARTITION);

    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "Falta la identidad AWS de esta placa en la particion %s (%s)."
                 " Grabarla con tools/aws_identity.py",
                 IDENTITY_PARTITION,
                 esp_err_to_name(result));
        return result;
    }

    ESP_LOGI(TAG, "Identidad AWS leida de la particion %s", IDENTITY_PARTITION);
    return ESP_OK;
}
#endif

esp_err_t MqttClient::publish_telemetry(const TelemetryData &measurement)
{
    // Descarte rápido para no llenar la cola mientras no hay broker. La
    // tarea vuelve a comprobarlo al procesar el mensaje.
    if (!connected_.load()) {
        return ESP_ERR_INVALID_STATE;
    }

    const Message message{MessageType::Publish, measurement};
    if (xQueueSend(queue_, &message, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

bool MqttClient::is_connected() const
{
    return connected_.load();
}

void MqttClient::task_entry(void *context)
{
    auto *mqtt_client = static_cast<MqttClient *>(context);
    mqtt_client->run();
}

void MqttClient::run()
{
    // Ver SensorTask::run(): misma suscripción desde la propia tarea.
    const esp_err_t wdt_result = esp_task_wdt_add(nullptr);
    if (wdt_result != ESP_OK) {
        ESP_LOGE(TAG,
                 "No se pudo suscribir al watchdog: %s",
                 esp_err_to_name(wdt_result));
    }

    Message message{};

    while (true) {
        esp_task_wdt_reset();

        // Espera acotada por el watchdog, igual que TelemetryTask.
        if (xQueueReceive(queue_, &message, pdMS_TO_TICKS(QUEUE_WAIT_MS)) ==
            pdPASS) {
            handle_message(message);
        }
    }
}

void MqttClient::handle_message(const Message &message)
{
    switch (message.type) {
    case MessageType::NetworkUp:
        if (!transport_started_) {
            start_transport();
        } else {
            ESP_LOGI(TAG,
                     "Wi-Fi recuperado; MQTT continuara su reconexion automatica");
        }
        break;

    case MessageType::NetworkDown:
        /*
         * No se toca connected_: la sesión MQTT puede sobrevivir a un corte
         * breve de Wi-Fi, y en ese caso ESP-MQTT no emite ningún evento al
         * volver. Si acá se marcara "sin conexión", nada la volvería a
         * marcar como conectada. Si la sesión sí se cae, ESP-MQTT lo avisa
         * con MQTT_EVENT_DISCONNECTED (a más tardar al vencer el
         * keepalive). Mientras tanto las mediciones esperan en su outbox.
         */
        ESP_LOGW(TAG, "Wi-Fi no disponible; la sesion MQTT decide si sigue viva");
        break;

    case MessageType::BrokerConnected:
        connected_.store(true);
        ESP_LOGI(TAG, "Conectado al broker MQTT");
        break;

    case MessageType::BrokerDisconnected:
        connected_.store(false);
        break;

    case MessageType::Publish:
        send_telemetry(message.measurement);
        break;
    }
}

void MqttClient::start_transport()
{
    ESP_LOGI(TAG, "Direccion IP disponible; conectando al broker");
    const esp_err_t result = esp_mqtt_client_start(client_);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar MQTT: %s", esp_err_to_name(result));
        return;
    }
    transport_started_ = true;
}

void MqttClient::send_telemetry(const TelemetryData &measurement)
{
    // La conexión pudo caerse entre publish_telemetry() y este momento.
    if (!connected_.load()) {
        ESP_LOGW(TAG,
                 "Medicion %" PRIu32 " descartada: sin conexion al broker",
                 measurement.sequence);
        return;
    }

    /*
     * La señal no es parte de la medición del sensor: es el estado de la
     * red al momento de enviar, así que se lee acá y no viaja en
     * TelemetryData. Si no se puede leer, el campo se omite; el gateway lo
     * trata como opcional.
     */
    char rssi_field[24]{};
    int rssi_dbm = 0;
    if (esp_wifi_sta_get_rssi(&rssi_dbm) == ESP_OK) {
        std::snprintf(rssi_field, sizeof(rssi_field), ",\"rssi_dbm\":%d",
                      rssi_dbm);
    }

    char payload[JSON_BUFFER_SIZE]{};
    const int payload_length = std::snprintf(
        payload,
        sizeof(payload),
        "{\"device_id\":\"%s\",\"sequence\":%" PRIu32
        ",\"temperature_c\":%.1f,\"humidity_pct\":%.1f,"
        "\"uptime_ms\":%" PRIu64 "%s}",
        device_id_.data(),
        measurement.sequence,
        static_cast<double>(measurement.temperature_c),
        static_cast<double>(measurement.humidity_percent),
        measurement.elapsed_time_ms,
        rssi_field);

    if (payload_length < 0 ||
        static_cast<std::size_t>(payload_length) >= sizeof(payload)) {
        ESP_LOGE(TAG,
                 "Medicion %" PRIu32 " descartada: el JSON no entra en %d bytes",
                 measurement.sequence,
                 JSON_BUFFER_SIZE);
        return;
    }

    /*
     * enqueue entrega el mensaje al outbox y retorna sin esperar la red.
     * La tarea interna de ESP-MQTT realiza el envio y espera el ACK de QoS 1.
     */
    const int message_id = esp_mqtt_client_enqueue(
        client_,
        telemetry_topic_.data(),
        payload,
        payload_length,
        MQTT_QOS,
        MQTT_RETAIN,
        true);

    if (message_id < 0) {
        ESP_LOGE(TAG,
                 "Medicion %" PRIu32 " descartada: ESP-MQTT no la acepto",
                 measurement.sequence);
        return;
    }

    ESP_LOGI(TAG,
             "Telemetria encolada | Secuencia: %" PRIu32
             " | Message ID: %d",
             measurement.sequence,
             message_id);
}

void MqttClient::post_event(MessageType type)
{
    const Message message{type, {}};
    if (xQueueSend(queue_, &message, 0) != pdPASS) {
        ESP_LOGE(TAG,
                 "Cola MQTT llena: se perdio el evento %d",
                 static_cast<int>(type));
    }
}

/*
 * Los dos handlers corren en tareas ajenas: el de ESP-MQTT en su tarea
 * interna y el de red en la tarea de eventos de ESP-IDF. Por eso no tocan
 * el estado: solo loguean y le pasan el evento a la tarea MQTT.
 */
void MqttClient::mqtt_event_handler(void *context,
                                    esp_event_base_t,
                                    std::int32_t event_id,
                                    void *event_data)
{
    auto *mqtt_client = static_cast<MqttClient *>(context);
    auto *event = static_cast<esp_mqtt_event_handle_t>(event_data);

    switch (event_id) {
    case MQTT_EVENT_CONNECTED:
        mqtt_client->post_event(MessageType::BrokerConnected);
        break;

    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Desconectado del broker; ESP-MQTT reintentara");
        mqtt_client->post_event(MessageType::BrokerDisconnected);
        break;

    case MQTT_EVENT_ERROR:
        ESP_LOGE(TAG, "Error reportado por el cliente MQTT");
        mqtt_client->post_event(MessageType::BrokerDisconnected);
        break;

    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG,
                 "Publicacion QoS 1 confirmada | Message ID: %d",
                 event->msg_id);
        break;

    default:
        break;
    }
}

void MqttClient::network_event_handler(void *context,
                                       esp_event_base_t event_base,
                                       std::int32_t event_id,
                                       void *)
{
    auto *mqtt_client = static_cast<MqttClient *>(context);

    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        mqtt_client->post_event(MessageType::NetworkUp);
    } else if (event_base == WIFI_EVENT &&
               event_id == WIFI_EVENT_STA_DISCONNECTED) {
        mqtt_client->post_event(MessageType::NetworkDown);
    }
}
