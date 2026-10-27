#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "mqtt_client.hpp"
#include "oled_display.hpp"
#include "sensor_task.hpp"
#include "task_monitor.hpp"
#include "telemetry_data.hpp"
#include "telemetry_task.hpp"
#include "wifi_manager.hpp"

namespace {
constexpr char TAG[] = "AGRO_TELEMETRY";
constexpr UBaseType_t TELEMETRY_QUEUE_CAPACITY = 5;
}

extern "C" void app_main()
{
    ESP_LOGI(TAG, "Iniciando Agro Telemetry Firmware");
    ESP_LOGI(TAG,
             "Configuracion Wi-Fi | SSID: %s | Password: %s",
             CONFIG_AGRO_WIFI_SSID,
             CONFIG_AGRO_WIFI_PASSWORD[0] != '\0' ? "configurada" : "no configurada");

    // Prueba visual de la OLED, aislada del resto del firmware: si el
    // cableado I2C o la direccion estan mal, esto no debe impedir que
    // arranque el resto (Wi-Fi, MQTT, sensores).
    static OledDisplay oled_display{GPIO_NUM_21, GPIO_NUM_22};
    if (oled_display.init() == ESP_OK) {
        ESP_LOGI(TAG,
                 "OLED detectada: encendiendo todos los pixeles 1 segundo "
                 "como prueba");
        oled_display.fill();
        vTaskDelay(pdMS_TO_TICKS(1000));
        oled_display.clear();
    } else {
        ESP_LOGW(TAG, "OLED no disponible; se continua sin pantalla");
    }

    static WifiManager wifi_manager;
    const esp_err_t wifi_result = wifi_manager.start();
    if (wifi_result != ESP_OK) {
        ESP_LOGE(TAG,
                 "No se pudo iniciar WifiManager: %s",
                 esp_err_to_name(wifi_result));
        return;
    }

    static MqttClient mqtt_client{wifi_manager};
    const esp_err_t mqtt_result = mqtt_client.start();
    if (mqtt_result != ESP_OK) {
        ESP_LOGE(TAG,
                 "No se pudo iniciar MqttClient: %s",
                 esp_err_to_name(mqtt_result));
        return;
    }

    // La cola guarda cinco estructuras completas, no direcciones a estructuras.
    static QueueHandle_t telemetry_queue =
        xQueueCreate(TELEMETRY_QUEUE_CAPACITY, sizeof(TelemetryData));

    if (telemetry_queue == nullptr) {
        ESP_LOGE(TAG, "No se pudo crear la cola de telemetria");
        return;
    }

    static SensorTask sensor_task{telemetry_queue};
    static TelemetryTask telemetry_task{
        telemetry_queue, mqtt_client, oled_display};

    const esp_err_t telemetry_result = telemetry_task.start();
    if (telemetry_result != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo crear TelemetryTask");
        return;
    }

    const esp_err_t sensor_result = sensor_task.start();
    if (sensor_result != ESP_OK) {
        ESP_LOGE(TAG, "No se pudo crear SensorTask");
        return;
    }

    // Diagnóstico: igual que la OLED, si no arranca el resto sigue.
    static TaskMonitor task_monitor;
    if (task_monitor.start() != ESP_OK) {
        ESP_LOGW(TAG, "No se pudo crear TaskMonitor; se continua sin reporte");
    }

    ESP_LOGI(TAG, "Cola y tareas creadas correctamente");
}
