#pragma once

#include <array>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ota_job.hpp"

class MqttClient;

/**
 * Descarga e instala el firmware que pide un Job de AWS IoT, en una tarea
 * propia: bajar ~1 MB por HTTPS y escribirlo en flash lleva decenas de
 * segundos, y la tarea MQTT no puede quedar frenada mientras tanto.
 *
 * MqttClient recibe el Job, lo valida y lo deja en la cola. Esta tarea
 * informa el avance a través de MqttClient y, si la imagen quedó bien,
 * reinicia. El SUCCEEDED lo informa el firmware nuevo, cuando se confirma.
 */
class OtaUpdater {
public:
    OtaUpdater(QueueHandle_t job_queue, MqttClient &mqtt_client);
    ~OtaUpdater();

    OtaUpdater(const OtaUpdater &) = delete;
    OtaUpdater &operator=(const OtaUpdater &) = delete;

    esp_err_t start();

private:
    static void task_entry(void *context);
    void run();
    void process(const OtaJob &job);
    // nullptr si la imagen quedó instalada; si no, el motivo para AWS.
    const char *download_and_install(const OtaJob &job);
    bool rolled_back_from(const OtaJob &job) const;
    void report(const OtaJob &job, JobStatus status, const char *reason);

    QueueHandle_t job_queue_;
    MqttClient &mqtt_client_;
    TaskHandle_t task_handle_{nullptr};
    // 2,2 KB: fuera del stack de la tarea.
    OtaJob job_{};
    // AWS puede volver a entregar el mismo Job (por ejemplo, al reconectar)
    // y no tiene que procesarse dos veces en el mismo arranque.
    std::array<char, 65> last_job_id_{};
};
