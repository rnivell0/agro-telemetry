#include "device_id.hpp"

#include <cstdint>
#include <cstdio>

#include "esp_log.h"
#include "esp_mac.h"

namespace {
constexpr char TAG[] = "DEVICE_ID";
}

esp_err_t read_device_id(DeviceId &out)
{
    std::uint8_t mac[6]{};
    const esp_err_t result = esp_efuse_mac_get_default(mac);
    if (result != ESP_OK) {
        return result;
    }

    /*
     * Los primeros 3 bytes identifican al fabricante y se repiten entre
     * placas; los últimos 3 son los que cambian. Dos placas de Espressif
     * con distinto prefijo de fabricante podrían coincidir en estos 3
     * bytes, algo muy improbable con un puñado de nodos.
     */
    std::snprintf(out.data(),
                  out.size(),
                  "esp32-%02x%02x%02x",
                  mac[3],
                  mac[4],
                  mac[5]);

    ESP_LOGI(TAG,
             "Identidad del nodo: %s (MAC %02x:%02x:%02x:%02x:%02x:%02x)",
             out.data(),
             mac[0],
             mac[1],
             mac[2],
             mac[3],
             mac[4],
             mac[5]);
    return ESP_OK;
}
