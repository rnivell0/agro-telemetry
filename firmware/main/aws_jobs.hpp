#pragma once

#include "ota_job.hpp"

enum class JobParseResult {
    // El documento no trae ninguna ejecución pendiente.
    NoJob,
    // No se puede ni identificar el Job: no hay a quién reportarle nada.
    Unidentifiable,
    // job.job_id es válido, pero el resto del documento no.
    InvalidDocument,
    Ok,
};

/**
 * Lee la ejecución que traen $next/get/accepted y notify-next:
 *
 *   {"execution": {"jobId": "...", "jobDocument": {"operation": "ota",
 *     "version": "...", "sha256": "<64 hex>", "url": "https://..."}}}
 *
 * Corre en la tarea de ESP-MQTT: solo interpreta y valida, no toca estado.
 */
JobParseResult parse_job_execution(const char *data, int length, OtaJob &job);
