#include "sensor_task.hpp"

#include <cinttypes>
#include <cstdint>

#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "telemetry_data.hpp"

namespace {
constexpr char TAG[] = "SENSOR";
constexpr std::uint32_t TASK_STACK_SIZE = 2048;
constexpr UBaseType_t TASK_PRIORITY = 5;
constexpr std::uint32_t SENSOR_PERIOD_MS = 2000;
constexpr gpio_num_t DHT22_DATA_PIN = GPIO_NUM_4;

// La tarea alimenta al watchdog una vez por período: con menos de la mitad
// del timeout queda margen para una lectura lenta sin reiniciar la placa.
static_assert(SENSOR_PERIOD_MS < CONFIG_ESP_TASK_WDT_TIMEOUT_S * 1000U / 2,
              "SENSOR_PERIOD_MS tiene que ser menor a la mitad del timeout "
              "del task watchdog");
}

SensorTask::SensorTask(QueueHandle_t telemetry_queue)
    : telemetry_queue_{telemetry_queue},
      dht22_sensor_{DHT22_DATA_PIN}
{
}

SensorTask::~SensorTask()
{
    if (task_handle_ != nullptr) {
        // ESP-IDF pide desuscribir la tarea del watchdog antes de borrarla.
        esp_task_wdt_delete(task_handle_);
        vTaskDelete(task_handle_);
    }
}

esp_err_t SensorTask::start()
{
    if (task_handle_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const BaseType_t result = xTaskCreate(
        task_entry,
        "SensorTask",
        TASK_STACK_SIZE,
        this,
        TASK_PRIORITY,
        &task_handle_);

    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void SensorTask::task_entry(void *context)
{
    auto *task = static_cast<SensorTask *>(context);
    task->run();
}

void SensorTask::run()
{
    /*
     * La suscripción se hace desde la propia tarea (nullptr = "la tarea que
     * llama"): así el watchdog empieza a contar recién cuando la tarea ya
     * está corriendo. Si falla, se sigue sin watchdog en vez de no medir.
     */
    const esp_err_t wdt_result = esp_task_wdt_add(nullptr);
    if (wdt_result != ESP_OK) {
        ESP_LOGE(TAG,
                 "No se pudo suscribir al watchdog: %s",
                 esp_err_to_name(wdt_result));
    }

    std::uint32_t sequence = 0;
    TickType_t last_wake_time = xTaskGetTickCount();

    while (true) {
        esp_task_wdt_reset();

        Dht22Reading reading{};
        const esp_err_t read_result = dht22_sensor_.read(reading);

        if (read_result != ESP_OK) {
            ESP_LOGW(TAG,
                     "Lectura de DHT22 fallida: %s",
                     esp_err_to_name(read_result));
        } else {
            TelemetryData measurement{
                .sequence = sequence,
                .temperature_c = reading.temperature_c,
                .humidity_percent = reading.humidity_percent,
                .elapsed_time_ms = static_cast<std::uint64_t>(
                                       esp_timer_get_time()) /
                                   1000ULL,
            };

            /*
             * Espera cero ticks: si las cinco posiciones están ocupadas, no
             * bloqueamos SensorTask y notificamos que esta muestra fue
             * descartada.
             */
            if (xQueueSend(telemetry_queue_, &measurement, 0) != pdPASS) {
                ESP_LOGW(TAG,
                         "Cola llena: medicion %" PRIu32 " descartada",
                         measurement.sequence);
            } else {
                ESP_LOGI(TAG,
                         "Medicion %" PRIu32 " enviada a TelemetryTask",
                         measurement.sequence);
            }

            ++sequence;
        }

        /*
         * A diferencia de vTaskDelay(), vTaskDelayUntil() toma como referencia
         * el instante programado anterior y evita acumular tiempo de ejecución.
         * El DHT22 admite como máximo una lectura cada 2 segundos: este
         * período está justo en ese límite.
         */
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(SENSOR_PERIOD_MS));
    }
}
