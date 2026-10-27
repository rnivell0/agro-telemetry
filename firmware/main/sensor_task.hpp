#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "dht22_sensor.hpp"

class SensorTask {
public:
    explicit SensorTask(QueueHandle_t telemetry_queue);
    ~SensorTask();

    SensorTask(const SensorTask &) = delete;
    SensorTask &operator=(const SensorTask &) = delete;

    esp_err_t start();

private:
    static void task_entry(void *context);
    void run();

    QueueHandle_t telemetry_queue_;
    TaskHandle_t task_handle_{nullptr};
    Dht22Sensor dht22_sensor_;
};
