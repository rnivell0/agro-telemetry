#include "mqtt_client.hpp"

#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "wifi_manager.hpp"
#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
#include "aws_jobs.hpp"
#endif

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

/*
 * Device Shadow: desired es lo que pide la nube, reported lo que hace el
 * nodo. El nodo pide el documento al conectarse (get) y AWS le avisa cada
 * cambio de desired (update/delta).
 *
 * Jobs: AWS avisa cuando cambia el próximo Job pendiente (notify-next), y
 * el nodo lo pide al conectarse ($next/get) por si llegó mientras no
 * estaba. El '+' es el comodín MQTT: un rechazo de cualquier Job propio.
 */
constexpr const char *AWS_SUBSCRIPTIONS[] = {
    "shadow/get/accepted",
    "shadow/get/rejected",
    "shadow/update/delta",
    "shadow/update/rejected",
    "jobs/notify-next",
    "jobs/$next/get/accepted",
    "jobs/$next/get/rejected",
    "jobs/+/update/rejected",
};
// Un documento de Job trae la URL firmada (~1,5 KB): con el buffer de
// 1 KB por defecto, ESP-MQTT lo entregaría partido en varios eventos.
constexpr int MQTT_RX_BUFFER_SIZE = 4096;
constexpr int MQTT_TX_BUFFER_SIZE = 1024;
// "$aws/things/<ID>/jobs/<jobId de hasta 64>/update"
constexpr std::size_t JOB_UPDATE_TOPIC_SIZE = 128;
constexpr char SHADOW_INTERVAL_KEY[] = "telemetry_interval_s";
// El mínimo es el período del sensor: más seguido no hay mediciones nuevas.
constexpr std::uint32_t MIN_TELEMETRY_INTERVAL_S = 2;
constexpr std::uint32_t MAX_TELEMETRY_INTERVAL_S = 3600;
// Las mediciones llegan cada 2 s con unos ms de variación. Sin margen, con
// un intervalo de 4 s una que llega 1 ms antes se saltearía y el envío
// quedaría a 6 s.
constexpr std::uint64_t SEND_TOLERANCE_MS = 500;

/*
 * get/accepted trae el documento entero (state.desired, state.reported);
 * update/delta trae solo lo que difiere, directo en state. Devuelve 0 si
 * no pide un intervalo o si el pedido no es válido.
 */
std::uint32_t parse_telemetry_interval(const char *data,
                                       int length,
                                       bool in_desired)
{
    cJSON *document = cJSON_ParseWithLength(data, length);
    if (document == nullptr) {
        ESP_LOGW(TAG, "El documento del shadow no es JSON; se ignora");
        return 0;
    }

    const cJSON *state = cJSON_GetObjectItemCaseSensitive(document, "state");
    if (in_desired) {
        state = cJSON_GetObjectItemCaseSensitive(state, "desired");
    }
    const cJSON *field =
        cJSON_GetObjectItemCaseSensitive(state, SHADOW_INTERVAL_KEY);

    std::uint32_t interval_s = 0;
    if (field != nullptr) {
        // El rango va primero: convertir a entero un double fuera de rango
        // es comportamiento indefinido.
        const double value = cJSON_IsNumber(field) ? field->valuedouble : -1.0;
        if (value >= MIN_TELEMETRY_INTERVAL_S &&
            value <= MAX_TELEMETRY_INTERVAL_S &&
            value == static_cast<double>(static_cast<std::uint32_t>(value))) {
            interval_s = static_cast<std::uint32_t>(value);
        } else {
            ESP_LOGW(TAG,
                     "%s pedido invalido (entero de %" PRIu32 " a %" PRIu32
                     " s); se mantiene el actual",
                     SHADOW_INTERVAL_KEY,
                     MIN_TELEMETRY_INTERVAL_S,
                     MAX_TELEMETRY_INTERVAL_S);
        }
    }

    cJSON_Delete(document);
    return interval_s;
}
#else
extern const char mqtt_ca_pem_start[] asm("_binary_mqtt_ca_pem_start");
#endif

/*
 * Una imagen recién llegada por OTA arranca en PENDING_VERIFY. Llegar al
 * broker prueba que funcionan Wi-Fi, TLS e identidad, que es lo necesario
 * para recibir otra OTA que la corrija: con eso se confirma. Si se reinicia
 * antes, el bootloader vuelve a la imagen anterior. Una imagen grabada por
 * USB no está a prueba y nada de esto actúa.
 *
 * Pero una imagen que arranca y nunca llega al broker no se reiniciaría
 * sola: quedaría sin confirmar y sin poder recibir otra OTA. Para eso está
 * el plazo. Es generoso porque una imagen sana con mala señal también
 * tarda, y descartarla por eso haría fallar un Job que estaba bien.
 */
constexpr std::uint64_t IMAGE_VERIFY_TIMEOUT_US = 5ULL * 60 * 1000 * 1000;
// Lo crea start() y lo borra la tarea MQTT al confirmar: nunca a la vez.
esp_timer_handle_t image_verify_timer = nullptr;

bool running_image_pending()
{
    esp_ota_img_states_t state{};
    return esp_ota_get_state_partition(esp_ota_get_running_partition(),
                                       &state) == ESP_OK &&
           state == ESP_OTA_IMG_PENDING_VERIFY;
}

void reject_running_image(void *)
{
    ESP_LOGE(TAG,
             "El firmware %s no llego al broker a tiempo: vuelta a la imagen "
             "anterior",
             esp_app_get_description()->version);
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void start_image_verification()
{
    if (!running_image_pending()) {
        return;
    }

    esp_timer_create_args_t timer_args{};
    timer_args.callback = &reject_running_image;
    timer_args.name = "image_verify";
    if (esp_timer_create(&timer_args, &image_verify_timer) != ESP_OK ||
        esp_timer_start_once(image_verify_timer, IMAGE_VERIFY_TIMEOUT_US) !=
            ESP_OK) {
        ESP_LOGE(TAG, "No se pudo armar el plazo para confirmar el firmware");
        return;
    }
    ESP_LOGW(TAG,
             "Firmware %s a prueba: tiene que llegar al broker en %" PRIu64
             " s",
             esp_app_get_description()->version,
             IMAGE_VERIFY_TIMEOUT_US / 1000000);
}

void confirm_running_image()
{
    if (!running_image_pending()) {
        return;
    }

    if (image_verify_timer != nullptr) {
        esp_timer_stop(image_verify_timer);
        esp_timer_delete(image_verify_timer);
        image_verify_timer = nullptr;
    }

    const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "No se pudo confirmar el firmware nuevo: %s",
                 esp_err_to_name(result));
        return;
    }
    ESP_LOGI(TAG,
             "Firmware %s confirmado: se cancela el rollback",
             esp_app_get_description()->version);
}
}

MqttClient::MqttClient(const WifiManager &wifi_manager,
                       const DeviceId &device_id)
    : wifi_manager_{wifi_manager}, device_id_{device_id}
{
    std::snprintf(telemetry_topic_.data(),
                  telemetry_topic_.size(),
                  "agro/devices/%s/telemetry",
                  device_id_.data());
#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
    static_assert(std::size(AWS_SUBSCRIPTIONS) == AWS_SUBSCRIPTION_COUNT);
    // El thing se llama igual que el device ID.
    std::snprintf(thing_prefix_.data(),
                  thing_prefix_.size(),
                  "$aws/things/%s/",
                  device_id_.data());
    for (std::size_t i = 0; i < AWS_SUBSCRIPTION_COUNT; ++i) {
        std::snprintf(subscription_filters_[i].data(),
                      subscription_filters_[i].size(),
                      "%s%s",
                      thing_prefix_.data(),
                      AWS_SUBSCRIPTIONS[i]);
    }
#endif
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

    start_image_verification();

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
    configuration.buffer.size = MQTT_RX_BUFFER_SIZE;
    configuration.buffer.out_size = MQTT_TX_BUFFER_SIZE;
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

void MqttClient::subscribe_aws_topics()
{
    std::array<esp_mqtt_topic_t, AWS_SUBSCRIPTION_COUNT> topics{};
    for (std::size_t i = 0; i < AWS_SUBSCRIPTION_COUNT; ++i) {
        topics[i] = {subscription_filters_[i].data(), MQTT_QOS};
    }

    /*
     * Un solo SUBSCRIBE para todos: un único SUBACK avisa que están todos,
     * y recién ahí se piden el shadow y el próximo Job. Si se pidieran
     * antes, la respuesta podría llegar a un topic sin suscripción.
     */
    if (esp_mqtt_client_subscribe_multiple(
            client_, topics.data(), static_cast<int>(topics.size())) < 0) {
        ESP_LOGE(TAG, "No se pudo suscribir al shadow y a Jobs");
    }
}

void MqttClient::request_shadow()
{
    char topic[AWS_TOPIC_SIZE]{};
    std::snprintf(topic, sizeof(topic), "%sshadow/get", thing_prefix_.data());
    if (esp_mqtt_client_enqueue(client_, topic, "", 0, MQTT_QOS, false, true) <
        0) {
        ESP_LOGE(TAG, "No se pudo pedir el Device Shadow");
    }
}

void MqttClient::request_next_job()
{
    char topic[AWS_TOPIC_SIZE]{};
    std::snprintf(
        topic, sizeof(topic), "%sjobs/$next/get", thing_prefix_.data());
    if (esp_mqtt_client_enqueue(client_, topic, "{}", 2, MQTT_QOS, false, true) <
        0) {
        ESP_LOGE(TAG, "No se pudo pedir el proximo Job");
    }
}

void MqttClient::apply_telemetry_interval(std::uint32_t requested_interval_s)
{
    if (requested_interval_s != 0 &&
        requested_interval_s != telemetry_interval_s_) {
        ESP_LOGI(TAG,
                 "Intervalo de envio a AWS: %" PRIu32 " s -> %" PRIu32 " s",
                 telemetry_interval_s_,
                 requested_interval_s);
        telemetry_interval_s_ = requested_interval_s;
    }

    // Siempre se reporta, aunque no cambie nada: así reported refleja lo
    // que el nodo hace de verdad, también la primera vez y después de un
    // pedido inválido.
    report_shadow();
}

void MqttClient::report_shadow()
{
    char topic[AWS_TOPIC_SIZE]{};
    std::snprintf(
        topic, sizeof(topic), "%sshadow/update", thing_prefix_.data());

    // La versión sale de git describe al compilar (hasta 31 caracteres):
    // así se ve desde la nube qué firmware corre cada placa.
    char payload[128]{};
    const int payload_length = std::snprintf(
        payload,
        sizeof(payload),
        "{\"state\":{\"reported\":{\"%s\":%" PRIu32
        ",\"firmware_version\":\"%s\"}}}",
        SHADOW_INTERVAL_KEY,
        telemetry_interval_s_,
        esp_app_get_description()->version);

    if (esp_mqtt_client_enqueue(client_,
                                topic,
                                payload,
                                payload_length,
                                MQTT_QOS,
                                false,
                                true) < 0) {
        ESP_LOGE(TAG, "No se pudo reportar el estado al Device Shadow");
        return;
    }
    ESP_LOGI(TAG,
             "Shadow reportado | %s: %" PRIu32 " s | firmware: %s",
             SHADOW_INTERVAL_KEY,
             telemetry_interval_s_,
             esp_app_get_description()->version);
}

void MqttClient::set_ota_job_queue(QueueHandle_t ota_job_queue)
{
    ota_job_queue_ = ota_job_queue;
}

void MqttClient::report_job_status(const JobUpdate &update)
{
    const Message message{MessageType::JobStatusUpdate, {}, 0, update};
    if (xQueueSend(queue_, &message, 0) != pdPASS) {
        ESP_LOGE(TAG,
                 "Cola MQTT llena: se perdio el estado del Job %s",
                 update.job_id.data());
    }
}

void MqttClient::publish_job_status(const JobUpdate &update)
{
    static constexpr const char *STATUS_NAMES[] = {
        "IN_PROGRESS", "SUCCEEDED", "FAILED"};
    const char *const status =
        STATUS_NAMES[static_cast<std::size_t>(update.status)];

    char topic[JOB_UPDATE_TOPIC_SIZE]{};
    std::snprintf(topic,
                  sizeof(topic),
                  "%sjobs/%s/update",
                  thing_prefix_.data(),
                  update.job_id.data());

    char payload[128]{};
    const int payload_length =
        update.reason != nullptr
            ? std::snprintf(payload,
                            sizeof(payload),
                            "{\"status\":\"%s\",\"statusDetails\":"
                            "{\"reason\":\"%s\"}}",
                            status,
                            update.reason)
            : std::snprintf(
                  payload, sizeof(payload), "{\"status\":\"%s\"}", status);

    if (esp_mqtt_client_enqueue(client_,
                                topic,
                                payload,
                                payload_length,
                                MQTT_QOS,
                                false,
                                true) < 0) {
        ESP_LOGE(TAG,
                 "No se pudo informar el Job %s a AWS",
                 update.job_id.data());
        return;
    }
    ESP_LOGI(TAG,
             "Job %s informado: %s%s%s",
             update.job_id.data(),
             status,
             update.reason != nullptr ? " | " : "",
             update.reason != nullptr ? update.reason : "");
}

void MqttClient::handle_aws_data(const esp_mqtt_event_t &event)
{
    // Un documento más grande que el buffer llega en varios eventos; los
    // de este nodo entran en uno, así que uno partido se descarta entero.
    if (event.current_data_offset != 0) {
        return;
    }
    if (event.data_len != event.total_data_len) {
        ESP_LOGW(TAG,
                 "Documento de %d bytes partido en varios eventos; se ignora",
                 event.total_data_len);
        return;
    }

    const std::string_view topic = aws_topic_suffix(event);
    if (topic.starts_with("shadow/")) {
        handle_shadow_data(topic, event);
    } else if (topic == "jobs/notify-next" ||
               topic == "jobs/$next/get/accepted") {
        handle_job_data(event);
    } else if (topic.starts_with("jobs/") && topic.ends_with("/rejected")) {
        // $next/get/rejected o jobs/<id>/update/rejected. Las respuestas
        // accepted de un update también llegan, sin suscribirse, y no
        // requieren nada.
        ESP_LOGE(TAG,
                 "AWS rechazo un pedido de Jobs (%.*s): %.*s",
                 static_cast<int>(topic.size()),
                 topic.data(),
                 event.data_len,
                 event.data);
    }
}

void MqttClient::handle_shadow_data(std::string_view topic,
                                    const esp_mqtt_event_t &event)
{
    if (topic == "shadow/update/delta") {
        // Un pedido inválido no se aplica ni se reporta: desired y reported
        // siguen distintos y el delta queda visible en la consola.
        const std::uint32_t interval_s =
            parse_telemetry_interval(event.data, event.data_len, false);
        if (interval_s != 0) {
            post_event(MessageType::ShadowState, interval_s);
        }
    } else if (topic == "shadow/get/accepted") {
        post_event(MessageType::ShadowState,
                   parse_telemetry_interval(event.data, event.data_len, true));
    } else if (topic == "shadow/get/rejected") {
        // La primera vez es un 404: el shadow todavía no existe, y lo crea
        // el primer reporte.
        ESP_LOGI(TAG,
                 "No hay Device Shadow que leer: %.*s",
                 event.data_len,
                 event.data);
        post_event(MessageType::ShadowState);
    } else if (topic == "shadow/update/rejected") {
        ESP_LOGE(TAG,
                 "AWS rechazo el reporte del shadow: %.*s",
                 event.data_len,
                 event.data);
    }
}

void MqttClient::handle_job_data(const esp_mqtt_event_t &event)
{
    switch (parse_job_execution(event.data, event.data_len, pending_job_)) {
    case JobParseResult::NoJob:
        ESP_LOGI(TAG, "No hay Jobs pendientes");
        return;

    case JobParseResult::Unidentifiable:
        // Sin un jobId válido no hay topic al que reportar.
        return;

    case JobParseResult::InvalidDocument:
        report_job_status(JobUpdate{
            pending_job_.job_id, JobStatus::Failed, "invalid_document"});
        return;

    case JobParseResult::Ok:
        break;
    }

    /*
     * Si ya corre la versión que pide el Job, no hay nada que descargar:
     * o es el arranque después de la OTA (la imagen ya se confirmó al
     * conectar, antes de suscribirse) o el Job pedía la versión actual.
     */
    if (std::strncmp(pending_job_.version.data(),
                     esp_app_get_description()->version,
                     pending_job_.version.size()) == 0) {
        report_job_status(
            JobUpdate{pending_job_.job_id, JobStatus::Succeeded, nullptr});
        return;
    }

    if (ota_job_queue_ == nullptr ||
        xQueueSend(ota_job_queue_, &pending_job_, 0) != pdPASS) {
        ESP_LOGW(TAG,
                 "Job %s ignorado: ya hay una OTA en curso",
                 pending_job_.job_id.data());
    }
}

std::string_view
MqttClient::aws_topic_suffix(const esp_mqtt_event_t &event) const
{
    if (event.topic == nullptr) {
        return {};
    }
    // El topic del evento no termina en '\0': se usa su longitud.
    const std::string_view topic(event.topic,
                                 static_cast<std::size_t>(event.topic_len));
    const std::string_view prefix(thing_prefix_.data());
    if (!topic.starts_with(prefix)) {
        return {};
    }
    return topic.substr(prefix.size());
}
#endif

esp_err_t MqttClient::publish_telemetry(const TelemetryData &measurement)
{
    // Descarte rápido para no llenar la cola mientras no hay broker. La
    // tarea vuelve a comprobarlo al procesar el mensaje.
    if (!connected_.load()) {
        return ESP_ERR_INVALID_STATE;
    }

    const Message message{MessageType::Publish, measurement, 0};
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
        confirm_running_image();
#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
        // Sesión limpia: AWS no guarda las suscripciones de la conexión
        // anterior, y el desired o los Jobs pudieron cambiar mientras el
        // nodo no estaba.
        subscribe_aws_topics();
#endif
        break;

    case MessageType::BrokerDisconnected:
        connected_.store(false);
        break;

    case MessageType::Publish:
        send_telemetry(message.measurement);
        break;

#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
    case MessageType::AwsSubscribed:
        request_shadow();
        request_next_job();
        break;

    case MessageType::ShadowState:
        apply_telemetry_interval(message.telemetry_interval_s);
        break;

    case MessageType::JobStatusUpdate:
        publish_job_status(message.job_update);
        break;
#endif
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

#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
    // Sin log: con un intervalo de 60 s serían 29 líneas por cada envío.
    const std::uint64_t interval_ms = telemetry_interval_s_ * 1000ULL;
    if (last_sent_ms_ != 0 &&
        measurement.elapsed_time_ms - last_sent_ms_ + SEND_TOLERANCE_MS <
            interval_ms) {
        return;
    }
#endif

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
#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
    last_sent_ms_ = measurement.elapsed_time_ms;
#endif

    ESP_LOGI(TAG,
             "Telemetria encolada | Secuencia: %" PRIu32
             " | Message ID: %d",
             measurement.sequence,
             message_id);
}

void MqttClient::post_event(MessageType type,
                            std::uint32_t telemetry_interval_s)
{
    const Message message{type, {}, telemetry_interval_s};
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

#if CONFIG_AGRO_MQTT_BACKEND_AWS_IOT
    // El único SUBSCRIBE es el del shadow y Jobs, así que no hace falta
    // mirar el msg_id.
    case MQTT_EVENT_SUBSCRIBED:
        if (event->error_handle != nullptr &&
            event->error_handle->error_type ==
                MQTT_ERROR_TYPE_SUBSCRIBE_FAILED) {
            ESP_LOGE(TAG,
                     "AWS rechazo la suscripcion al shadow o a Jobs: revisar "
                     "la politica del thing");
        } else {
            mqtt_client->post_event(MessageType::AwsSubscribed);
        }
        break;

    case MQTT_EVENT_DATA:
        mqtt_client->handle_aws_data(*event);
        break;
#endif

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
