#include "dht22_sensor.hpp"

#include <cstdint>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

namespace {

constexpr char TAG[] = "DHT22";

// El datasheet pide entre 0.8 y 20 ms en bajo, típico 1 ms. Con el tick de
// 10 ms de este firmware vTaskDelay no puede medir eso, así que se espera
// activamente.
constexpr std::uint32_t START_SIGNAL_US = 1100;

/**
 * Espera hasta que el pin alcance el nivel indicado, con timeout en
 * microsegundos.
 */
esp_err_t wait_for_level(gpio_num_t pin, int level, int timeout_us)
{
    const std::int64_t start = esp_timer_get_time();

    while (gpio_get_level(pin) != level) {
        if (esp_timer_get_time() - start > timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
    }
    return ESP_OK;
}

} // namespace

Dht22Sensor::Dht22Sensor(gpio_num_t data_pin) : data_pin_{data_pin}
{
}

esp_err_t Dht22Sensor::read(Dht22Reading &out_reading) const
{
    std::uint8_t bytes[5] = {0, 0, 0, 0, 0};

    /*
     * Pin en drenaje abierto con la entrada habilitada: escribir 0 baja la
     * línea, escribir 1 la suelta (la sube el pull-up) y se puede leer sin
     * reconfigurar. El driver anterior pasaba de salida a entrada con
     * gpio_config() justo después de la señal de inicio; con el DHT22, que
     * responde más rápido, esa demora hacía perder el pulso de respuesta y
     * leer toda la trama corrida un bit.
     */
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << data_pin_,
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&config);

    // 1) Señal de inicio: el ESP32 baja la línea algo más de 1 ms.
    gpio_set_level(data_pin_, 0);
    esp_rom_delay_us(START_SIGNAL_US);

    /*
     * El resto de la lectura dura unos 5 ms y es sensible a interrupciones
     * (Wi-Fi, etc.) que podrían distorsionar la medición de los pulsos. Se
     * deshabilitan antes de soltar la línea para no perder la respuesta.
     */
    portDISABLE_INTERRUPTS();

    // 2) A partir de aquí el DHT22 toma el control de la línea; el ESP32
    // solo escucha.
    gpio_set_level(data_pin_, 1);

    esp_err_t result = ESP_OK;
    const char *failure_stage = nullptr;
    int failed_bit = -1;

    do {
        // La línea sube por el pull-up y el sensor la vuelve a bajar a los
        // 20-40 us. Esperar primero el alto evita tomar como respuesta el
        // propio bajo de la señal de inicio.
        if (wait_for_level(data_pin_, 1, 50) != ESP_OK) {
            result = ESP_ERR_TIMEOUT;
            failure_stage = "la linea no subio al soltarla (posible falta "
                             "de pull-up o cortocircuito a GND)";
            break;
        }
        // Respuesta del sensor: 80 us en bajo seguidos de 80 us en alto.
        if (wait_for_level(data_pin_, 0, 90) != ESP_OK) {
            result = ESP_ERR_TIMEOUT;
            failure_stage = "el sensor nunca bajo la linea (posible falta "
                             "de alimentacion o cableado)";
            break;
        }
        if (wait_for_level(data_pin_, 1, 90) != ESP_OK) {
            result = ESP_ERR_TIMEOUT;
            failure_stage = "el sensor no completo su pulso de respuesta";
            break;
        }
        if (wait_for_level(data_pin_, 0, 90) != ESP_OK) {
            result = ESP_ERR_TIMEOUT;
            failure_stage = "el sensor no inicio el envio de datos";
            break;
        }

        // 40 bits de datos. Cada bit empieza con ~50 us en bajo seguidos de
        // un pulso en alto cuya duración indica el valor: ~26 us para un
        // '0' y ~70 us para un '1'. Se espera el flanco de subida, se dejan
        // pasar 40 us y se mira el nivel: si ya bajó, era un '0'; si sigue
        // en alto, era un '1'. Después del bit 40 el sensor también baja la
        // línea, así que el flanco de bajada se exige en todos los bits.
        for (int bit = 0; bit < 40; ++bit) {
            if (wait_for_level(data_pin_, 1, 90) != ESP_OK) {
                result = ESP_ERR_TIMEOUT;
                failure_stage = "flanco a alto del bit";
                failed_bit = bit;
                break;
            }

            esp_rom_delay_us(40);

            const int byte_index = bit / 8;
            bytes[byte_index] <<= 1;
            if (gpio_get_level(data_pin_) != 0) {
                bytes[byte_index] |= 1;
            }

            if (wait_for_level(data_pin_, 0, 90) != ESP_OK) {
                result = ESP_ERR_TIMEOUT;
                failure_stage = "flanco a bajo del bit";
                failed_bit = bit;
                break;
            }
        }
    } while (false);

    portENABLE_INTERRUPTS();

    if (result != ESP_OK) {
        if (failed_bit >= 0) {
            ESP_LOGW(TAG,
                     "Timeout en %s (bit %d de 40)",
                     failure_stage,
                     failed_bit);
        } else {
            ESP_LOGW(TAG, "Timeout: %s", failure_stage);
        }
        return result;
    }

    const std::uint8_t checksum =
        static_cast<std::uint8_t>(bytes[0] + bytes[1] + bytes[2] + bytes[3]);
    if (checksum != bytes[4]) {
        ESP_LOGW(TAG,
                 "Checksum invalido: bytes=%02X %02X %02X %02X %02X "
                 "(esperado %02X)",
                 bytes[0],
                 bytes[1],
                 bytes[2],
                 bytes[3],
                 bytes[4],
                 checksum);
        return ESP_ERR_INVALID_CRC;
    }

    // Humedad y temperatura llegan en décimas, 16 bits cada una. La
    // temperatura usa signo y magnitud, no complemento a 2: el bit 15 es el
    // signo y los otros 15 el valor absoluto.
    const int humidity_tenths = (bytes[0] << 8) | bytes[1];
    const int temperature_magnitude = ((bytes[2] & 0x7F) << 8) | bytes[3];
    const int temperature_tenths = (bytes[2] & 0x80) != 0
                                       ? -temperature_magnitude
                                       : temperature_magnitude;

    out_reading.humidity_percent = static_cast<float>(humidity_tenths) / 10.0F;
    out_reading.temperature_c = static_cast<float>(temperature_tenths) / 10.0F;
    return ESP_OK;
}
