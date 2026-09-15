#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

class MqttClient;
class OledDisplay;

class TelemetryTask {
public:
    TelemetryTask(QueueHandle_t telemetry_queue,
                  MqttClient &mqtt_client,
                  OledDisplay &oled_display);
    ~TelemetryTask();

    // Una tarea representa un único recurso, por eso no se puede copiar.
    TelemetryTask(const TelemetryTask &) = delete;
    TelemetryTask &operator=(const TelemetryTask &) = delete;

    esp_err_t start();

private:
    static void task_entry(void *context);
    void run();

    QueueHandle_t telemetry_queue_;
    MqttClient &mqtt_client_;
    OledDisplay &oled_display_;
    TaskHandle_t task_handle_{nullptr};
};
