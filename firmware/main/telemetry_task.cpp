#include "telemetry_task.hpp"

#include <cinttypes>
#include <cstdint>
#include <cstdio>

#include "esp_log.h"
#include "mqtt_client.hpp"
#include "oled_display.hpp"
#include "telemetry_data.hpp"

namespace {
constexpr char TAG[] = "TELEMETRY";
constexpr std::uint32_t TASK_STACK_SIZE = 3072;
constexpr UBaseType_t TASK_PRIORITY = 5;
}

TelemetryTask::TelemetryTask(QueueHandle_t telemetry_queue,
                             MqttClient &mqtt_client,
                             OledDisplay &oled_display)
    : telemetry_queue_{telemetry_queue},
      mqtt_client_{mqtt_client},
      oled_display_{oled_display}
{
}

TelemetryTask::~TelemetryTask()
{
    /*
     * RAII: si el objeto deja de existir, también liberamos la tarea que posee.
     * En este firmware la instancia es static y normalmente vive para siempre.
     */
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
    }
}

esp_err_t TelemetryTask::start()
{
    if (task_handle_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * xTaskCreate sigue siendo la API nativa C de FreeRTOS. Le pasamos `this`
     * como contexto para conectar la tarea con esta instancia C++.
     */
    const BaseType_t result = xTaskCreate(
        task_entry,
        "TelemetryTask",
        TASK_STACK_SIZE,
        this,
        TASK_PRIORITY,
        &task_handle_);

    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void TelemetryTask::task_entry(void *context)
{
    /*
     * FreeRTOS exige una función C de tipo void (*)(void *). Esta función
     * estática actúa como puente hacia el método de la instancia.
     */
    auto *task = static_cast<TelemetryTask *>(context);
    task->run();
}

void TelemetryTask::run()
{
    TelemetryData measurement{};

    while (true) {
        /*
         * portMAX_DELAY mantiene la tarea bloqueada hasta que haya un elemento.
         * No hacemos polling ni consumimos CPU mientras la cola está vacía.
         */
        if (xQueueReceive(telemetry_queue_, &measurement, portMAX_DELAY) == pdPASS) {
            ESP_LOGI(TAG,
                     "Secuencia: %" PRIu32
                     " | Temperatura: %.1f C | Humedad: %.1f %%"
                     " | Tiempo: %" PRIu64 " ms",
                     measurement.sequence,
                     static_cast<double>(measurement.temperature_c),
                     static_cast<double>(measurement.humidity_percent),
                     measurement.elapsed_time_ms);

            const esp_err_t publish_result =
                mqtt_client_.publish_telemetry(measurement);
            if (publish_result != ESP_OK) {
                ESP_LOGW(TAG,
                         "No se publico la medicion %" PRIu32 ": %s",
                         measurement.sequence,
                         esp_err_to_name(publish_result));
            }

            // La fuente de la OLED solo soporta mayusculas, digitos, '.',
            // ':' y '%'; por eso las etiquetas van en mayuscula.
            char line1[17];
            char line2[17];
            char line3[17];
            std::snprintf(line1,
                          sizeof(line1),
                          "TEMP:%.1fC",
                          static_cast<double>(measurement.temperature_c));
            std::snprintf(line2,
                          sizeof(line2),
                          "HUM:%.1f%%",
                          static_cast<double>(measurement.humidity_percent));
            std::snprintf(line3,
                          sizeof(line3),
                          "MQTT:%s",
                          mqtt_client_.is_connected() ? "OK" : "NO");
            oled_display_.show_lines({line1, line2, line3});
        }
    }
}
