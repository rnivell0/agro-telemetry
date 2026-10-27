#include "task_monitor.hpp"

#include <algorithm>
#include <cstdint>

#include "esp_log.h"
#include "esp_system.h"

#if configUSE_TRACE_FACILITY != 1
#error "TaskMonitor necesita CONFIG_FREERTOS_USE_TRACE_FACILITY=y (ver sdkconfig.defaults)"
#endif

namespace {
constexpr char TAG[] = "TASK_MONITOR";
constexpr std::uint32_t TASK_STACK_SIZE = 3072;
// Apenas por encima de las tareas IDLE: es diagnóstico y nunca debe
// quitarle CPU al muestreo ni a la red.
constexpr UBaseType_t TASK_PRIORITY = 1;
constexpr std::uint32_t REPORT_PERIOD_MS = 30000;

const char *state_name(eTaskState state)
{
    switch (state) {
    case eRunning:
        return "corriendo";
    case eReady:
        return "lista";
    case eBlocked:
        return "bloqueada";
    case eSuspended:
        return "suspendida";
    case eDeleted:
        return "borrada";
    default:
        return "invalida";
    }
}
}

TaskMonitor::~TaskMonitor()
{
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
    }
}

esp_err_t TaskMonitor::start()
{
    if (task_handle_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    const BaseType_t result = xTaskCreate(
        task_entry,
        "TaskMonitor",
        TASK_STACK_SIZE,
        this,
        TASK_PRIORITY,
        &task_handle_);

    return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void TaskMonitor::task_entry(void *context)
{
    auto *monitor = static_cast<TaskMonitor *>(context);
    monitor->run();
}

void TaskMonitor::run()
{
    while (true) {
        /*
         * vTaskDelay alcanza: que el reporte derive unos milisegundos no
         * importa. La espera va primero para que el primer reporte ya
         * incluya un rato de trabajo real de las demás tareas.
         */
        vTaskDelay(pdMS_TO_TICKS(REPORT_PERIOD_MS));
        log_report();
    }
}

void TaskMonitor::log_report()
{
    /*
     * uxTaskGetSystemState() frena el scheduler mientras recorre las listas
     * de tareas. Con pocas tareas y cada 30 s es un costo despreciable, pero
     * por eso no conviene llamarla en un loop rápido.
     */
    const UBaseType_t count = uxTaskGetSystemState(
        statuses_.data(), statuses_.size(), nullptr);

    if (count == 0) {
        ESP_LOGW(TAG,
                 "Hay mas de %u tareas; agrandar MAX_TASKS",
                 static_cast<unsigned>(MAX_TASKS));
        return;
    }

    // Primero las que menos margen tienen.
    std::sort(statuses_.begin(),
              statuses_.begin() + count,
              [](const TaskStatus_t &a, const TaskStatus_t &b) {
                  return a.usStackHighWaterMark < b.usStackHighWaterMark;
              });

    ESP_LOGI(TAG,
             "%u tareas | heap libre: %u B (minimo historico: %u B)",
             static_cast<unsigned>(count),
             static_cast<unsigned>(esp_get_free_heap_size()),
             static_cast<unsigned>(esp_get_minimum_free_heap_size()));

    for (UBaseType_t i = 0; i < count; ++i) {
        const TaskStatus_t &status = statuses_[i];

        /*
         * Prioridad actual/base: solo difieren si la tarea heredó prioridad
         * por tener tomado un mutex que pide otra más prioritaria.
         * En ESP-IDF la marca de agua viene en bytes, igual que el tamaño
         * que se le pasa a xTaskCreate.
         */
        ESP_LOGI(TAG,
                 "  %-16s %-10s prio %2u/%-2u stack libre min: %5u B",
                 status.pcTaskName,
                 state_name(status.eCurrentState),
                 static_cast<unsigned>(status.uxCurrentPriority),
                 static_cast<unsigned>(status.uxBasePriority),
                 static_cast<unsigned>(status.usStackHighWaterMark));
    }
}
