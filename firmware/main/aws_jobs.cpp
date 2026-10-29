#include "aws_jobs.hpp"

#include <cctype>
#include <cstddef>
#include <cstring>

#include "cJSON.h"
#include "esp_log.h"

namespace {
constexpr char TAG[] = "AWS_JOBS";
constexpr char OTA_OPERATION[] = "ota";
constexpr char URL_SCHEME[] = "https://";

// Copia un string no vacío que entre entero, con su '\0', en out.
bool copy_string(const cJSON *item, char *out, std::size_t size)
{
    if (!cJSON_IsString(item)) {
        return false;
    }
    const std::size_t length = std::strlen(item->valuestring);
    if (length == 0 || length >= size) {
        return false;
    }
    std::memcpy(out, item->valuestring, length + 1);
    return true;
}

// El jobId termina dentro de un topic: con '/', '+' o '#' podría apuntar a
// otro. AWS ya lo restringe a estos caracteres; acá no se confía en eso.
bool is_valid_job_id(const char *job_id)
{
    for (const char *c = job_id; *c != '\0'; ++c) {
        if (std::isalnum(static_cast<unsigned char>(*c)) == 0 && *c != '-' &&
            *c != '_') {
            return false;
        }
    }
    return true;
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool parse_sha256(const cJSON *item, std::array<std::uint8_t, 32> &out)
{
    if (!cJSON_IsString(item) ||
        std::strlen(item->valuestring) != out.size() * 2) {
        return false;
    }
    for (std::size_t i = 0; i < out.size(); ++i) {
        const int high = hex_value(item->valuestring[2 * i]);
        const int low = hex_value(item->valuestring[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        out[i] = static_cast<std::uint8_t>(high * 16 + low);
    }
    return true;
}

bool parse_ota_document(const cJSON *document, OtaJob &job)
{
    const cJSON *operation =
        cJSON_GetObjectItemCaseSensitive(document, "operation");
    if (!cJSON_IsString(operation) ||
        std::strcmp(operation->valuestring, OTA_OPERATION) != 0) {
        ESP_LOGW(TAG, "El Job no es de OTA");
        return false;
    }
    if (!copy_string(cJSON_GetObjectItemCaseSensitive(document, "version"),
                     job.version.data(),
                     job.version.size())) {
        ESP_LOGW(TAG, "El Job no trae una version valida");
        return false;
    }
    if (!parse_sha256(cJSON_GetObjectItemCaseSensitive(document, "sha256"),
                      job.image_sha256)) {
        ESP_LOGW(TAG, "El Job no trae un sha256 valido");
        return false;
    }
    if (!copy_string(cJSON_GetObjectItemCaseSensitive(document, "url"),
                     job.url.data(),
                     job.url.size()) ||
        std::strncmp(job.url.data(), URL_SCHEME, std::strlen(URL_SCHEME)) !=
            0) {
        ESP_LOGW(TAG, "El Job no trae una URL https que entre en %u bytes",
                 static_cast<unsigned>(job.url.size()));
        return false;
    }
    return true;
}
}

JobParseResult parse_job_execution(const char *data, int length, OtaJob &job)
{
    cJSON *root = cJSON_ParseWithLength(data, length);
    if (root == nullptr) {
        ESP_LOGW(TAG, "El documento del Job no es JSON");
        return JobParseResult::Unidentifiable;
    }

    JobParseResult result = JobParseResult::Ok;
    const cJSON *execution = cJSON_GetObjectItemCaseSensitive(root, "execution");
    if (execution == nullptr) {
        result = JobParseResult::NoJob;
    } else if (!copy_string(cJSON_GetObjectItemCaseSensitive(execution, "jobId"),
                            job.job_id.data(),
                            job.job_id.size()) ||
               !is_valid_job_id(job.job_id.data())) {
        ESP_LOGW(TAG, "El Job no trae un jobId valido");
        result = JobParseResult::Unidentifiable;
    } else if (!parse_ota_document(
                   cJSON_GetObjectItemCaseSensitive(execution, "jobDocument"),
                   job)) {
        result = JobParseResult::InvalidDocument;
    }

    cJSON_Delete(root);
    return result;
}
