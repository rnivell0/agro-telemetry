#pragma once

#include <array>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/**
 * Tarea de diagnóstico: cada cierto tiempo lista todas las tareas del
 * sistema, las propias y las que crea ESP-IDF (Wi-Fi, TCP/IP, MQTT, event
 * loop), con su estado, prioridad y el mínimo de stack libre que tuvieron
 * desde que arrancaron.
 */
class TaskMonitor {
public:
    TaskMonitor() = default;
    ~TaskMonitor();

    TaskMonitor(const TaskMonitor &) = delete;
    TaskMonitor &operator=(const TaskMonitor &) = delete;

    esp_err_t start();

private:
    // Estimación con margen: el primer reporte confirma cuántas hay.
    static constexpr UBaseType_t MAX_TASKS = 24;

    static void task_entry(void *context);
    void run();
    void log_report();

    TaskHandle_t task_handle_{nullptr};
    // Miembro y no variable local: son casi 1 KB que no conviene poner en
    // la pila de la propia tarea.
    std::array<TaskStatus_t, MAX_TASKS> statuses_{};
};
