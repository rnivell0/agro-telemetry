#pragma once

#include <array>
#include <cstdint>
#include <type_traits>

/**
 * Un Job de OTA ya validado, tal como MqttClient se lo pasa a OtaUpdater
 * por una cola: datos simples, copiados por valor.
 */
struct OtaJob {
    // AWS limita el jobId a 64 caracteres de [a-zA-Z0-9_-].
    std::array<char, 65> job_id;
    // Igual que esp_app_desc_t::version: hasta 31 caracteres.
    std::array<char, 32> version;
    // El SHA-256 que el build agrega al final de la imagen (los últimos 32
    // bytes del .bin), el mismo que devuelve esp_partition_get_sha256().
    std::array<std::uint8_t, 32> image_sha256;
    // URL firmada de S3. Con el token de sesión del rol de firma ronda
    // los 1,5 KB.
    std::array<char, 2048> url;
};
static_assert(std::is_trivially_copyable_v<OtaJob>,
              "La cola de FreeRTOS copia los Jobs byte a byte");

enum class JobStatus : std::uint8_t {
    InProgress,
    Succeeded,
    Failed,
};

// Lo que cualquier tarea le pide a MqttClient que informe sobre un Job.
struct JobUpdate {
    std::array<char, 65> job_id;
    JobStatus status;
    // Un literal (duración estática), o nullptr si no hay motivo que dar.
    const char *reason;
};
static_assert(std::is_trivially_copyable_v<JobUpdate>,
              "Viaja dentro de los mensajes de la cola de MqttClient");
