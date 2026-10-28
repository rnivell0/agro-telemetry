#include "ota_updater.hpp"

#include <cstdint>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"

#include "mqtt_client.hpp"

namespace {
constexpr char TAG[] = "OTA";
// Medido después con TaskMonitor: el handshake TLS con S3 corre en esta
// tarea y es lo que más stack pide.
constexpr std::uint32_t TASK_STACK_SIZE = 8192;
// Debajo de MqttClient y TelemetryTask: la descarga no tiene apuro y las
// mediciones siguen saliendo mientras tanto.
constexpr UBaseType_t TASK_PRIORITY = 3;
constexpr int HTTP_TIMEOUT_MS = 10000;
// La URL firmada va entera en la línea del GET; con el buffer de 512 B
// por defecto no entra.
constexpr int HTTP_TX_BUFFER_SIZE = 2560;
// Margen para que el log salga por el puerto serie antes de reiniciar.
constexpr std::uint32_t RESTART_DELAY_MS = 1000;
}

OtaUpdater::OtaUpdater(QueueHandle_t job_queue, MqttClient &mqtt_client)
    : job_queue_{job_queue}, mqtt_client_{mqtt_client}
{
}

OtaUpdater::~OtaUpdater()
{
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
    }
}

esp_err_t OtaUpdater::start()
{
    if (task_handle_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const BaseType_t created = xTaskCreate(task_entry,
                                           "OtaUpdater",
                                           TASK_STACK_SIZE,
                                           this,
                                           TASK_PRIORITY,
                                           &task_handle_);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void OtaUpdater::task_entry(void *context)
{
    auto *updater = static_cast<OtaUpdater *>(context);
    updater->run();
}

void OtaUpdater::run()
{
    /*
     * A diferencia de las otras tareas, esta no se suscribe al watchdog:
     * esp_https_ota_begin() y perform() bloquean en la red hasta
     * HTTP_TIMEOUT_MS, más que el timeout del watchdog. Un cuelgue acá no
     * deja la placa inservible: la imagen nueva no arranca hasta que la
     * descarga termina bien, y aun así arranca a prueba (rollback).
     */
    while (true) {
        if (xQueueReceive(job_queue_, &job_, portMAX_DELAY) == pdPASS) {
            process(job_);
        }
    }
}

void OtaUpdater::process(const OtaJob &job)
{
    if (std::strcmp(job.job_id.data(), last_job_id_.data()) == 0) {
        ESP_LOGI(TAG, "Job %s ya atendido en este arranque", job.job_id.data());
        return;
    }
    last_job_id_ = job.job_id;

    if (rolled_back_from(job)) {
        // Sin esto, la imagen vieja volvería a descargar la que falló, y la
        // placa quedaría en un ciclo de descarga, reinicio y rollback.
        ESP_LOGW(TAG,
                 "El firmware %s ya se probo y el bootloader volvio atras",
                 job.version.data());
        report(job, JobStatus::Failed, "rolled_back");
        return;
    }

    ESP_LOGI(TAG,
             "Job %s: actualizar de %s a %s",
             job.job_id.data(),
             esp_app_get_description()->version,
             job.version.data());
    report(job, JobStatus::InProgress, nullptr);

    const char *failure = download_and_install(job);
    if (failure != nullptr) {
        report(job, JobStatus::Failed, failure);
        return;
    }

    ESP_LOGI(TAG,
             "Firmware %s instalado; reiniciando para probarlo",
             job.version.data());
    vTaskDelay(pdMS_TO_TICKS(RESTART_DELAY_MS));
    esp_restart();
}

const char *OtaUpdater::download_and_install(const OtaJob &job)
{
    esp_http_client_config_t http{};
    http.url = job.url.data();
    // S3 no usa la CA de AWS IoT: el paquete de CAs raíz de ESP-IDF la
    // cubre sin atarse a una en particular.
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.timeout_ms = HTTP_TIMEOUT_MS;
    http.buffer_size_tx = HTTP_TX_BUFFER_SIZE;
    http.keep_alive_enable = true;

    // Sin bulk_flash_erase: la partición se borra de a 4 KB mientras se
    // escribe, en vez de bloquear segundos al empezar.
    esp_https_ota_config_t config{};
    config.http_config = &http;

    esp_https_ota_handle_t handle = nullptr;
    esp_err_t result = esp_https_ota_begin(&config, &handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo iniciar la descarga: %s",
                 esp_err_to_name(result));
        return "download_failed";
    }

    // La cabecera llega primero: si el binario no es la versión que dice el
    // Job, se corta antes de bajar el resto.
    esp_app_desc_t image{};
    result = esp_https_ota_get_img_desc(handle, &image);
    if (result != ESP_OK ||
        std::strncmp(image.version, job.version.data(), sizeof(image.version)) !=
            0) {
        ESP_LOGE(TAG,
                 "El binario es la version %s y el Job pide %s",
                 result == ESP_OK ? image.version : "(ilegible)",
                 job.version.data());
        esp_https_ota_abort(handle);
        return "version_mismatch";
    }

    int last_decile = -1;
    while ((result = esp_https_ota_perform(handle)) ==
           ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        const int size = esp_https_ota_get_image_size(handle);
        if (size > 0) {
            const int decile =
                esp_https_ota_get_image_len_read(handle) * 10 / size;
            if (decile != last_decile) {
                ESP_LOGI(TAG, "Descarga: %d %%", decile * 10);
                last_decile = decile;
            }
        }
    }
    if (result != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "La descarga se corto: %s", esp_err_to_name(result));
        esp_https_ota_abort(handle);
        return "download_failed";
    }

    // Verifica la imagen (incluido su SHA-256 interno) y la deja como la
    // próxima en arrancar.
    result = esp_https_ota_finish(handle);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "La imagen descargada no es valida: %s",
                 esp_err_to_name(result));
        return "invalid_image";
    }

    /*
     * El SHA-256 interno solo prueba que la imagen llegó entera. El del Job
     * prueba que es la que eligió quien creó el Job, y no otra que esté en
     * el bucket con el mismo nombre.
     */
    std::array<std::uint8_t, 32> installed_sha256{};
    if (esp_partition_get_sha256(esp_ota_get_boot_partition(),
                                 installed_sha256.data()) != ESP_OK ||
        installed_sha256 != job.image_sha256) {
        ESP_LOGE(TAG, "El SHA-256 de la imagen no coincide con el del Job");
        esp_ota_set_boot_partition(esp_ota_get_running_partition());
        return "sha256_mismatch";
    }
    return nullptr;
}

bool OtaUpdater::rolled_back_from(const OtaJob &job) const
{
    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    esp_app_desc_t description{};
    return invalid != nullptr &&
           esp_ota_get_partition_description(invalid, &description) ==
               ESP_OK &&
           std::strncmp(description.version,
                        job.version.data(),
                        sizeof(description.version)) == 0;
}

void OtaUpdater::report(const OtaJob &job, JobStatus status, const char *reason)
{
    mqtt_client_.report_job_status(JobUpdate{job.job_id, status, reason});
}
