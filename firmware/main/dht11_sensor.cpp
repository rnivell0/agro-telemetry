#include "dht11_sensor.hpp"

#include <cstdint>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr char TAG[] = "DHT11";

/**
 * Espera hasta que el pin alcance el nivel indicado, con timeout en
 * microsegundos. Si se pide, informa cuánto tiempo permaneció en el nivel
 * anterior antes del cambio (se usa para distinguir un bit '0' de un '1').
 */
esp_err_t wait_for_level(gpio_num_t pin,
                          int level,
                          int timeout_us,
                          int *elapsed_us = nullptr)
{
    const std::int64_t start = esp_timer_get_time();

    while (gpio_get_level(pin) != level) {
        if (esp_timer_get_time() - start > timeout_us) {
            return ESP_ERR_TIMEOUT;
        }
    }

    if (elapsed_us != nullptr) {
        *elapsed_us = static_cast<int>(esp_timer_get_time() - start);
    }
    return ESP_OK;
}

} // namespace

Dht11Sensor::Dht11Sensor(gpio_num_t data_pin) : data_pin_{data_pin}
{
}

esp_err_t Dht11Sensor::read(Dht11Reading &out_reading) const
{
    std::uint8_t bytes[5] = {0, 0, 0, 0, 0};

    // 1) Señal de inicio: el ESP32 pone el pin en bajo al menos 18 ms.
    const gpio_config_t output_config = {
        .pin_bit_mask = 1ULL << data_pin_,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&output_config);

    gpio_set_level(data_pin_, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(data_pin_, 1);
    esp_rom_delay_us(30);

    // 2) A partir de aquí el DHT11 toma el control de la línea; el ESP32
    // solo escucha.
    const gpio_config_t input_config = {
        .pin_bit_mask = 1ULL << data_pin_,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&input_config);

    /*
     * El resto de la lectura dura menos de 5 ms, pero es sensible a
     * interrupciones (Wi-Fi, etc.) que podrían distorsionar la medición de
     * los pulsos. Se deshabilitan brevemente para leer con precisión.
     */
    portDISABLE_INTERRUPTS();

    esp_err_t result = ESP_OK;
    const char *failure_stage = nullptr;
    int failed_bit = -1;

    do {
        // Respuesta del sensor: 80 us en bajo seguidos de 80 us en alto.
        if (wait_for_level(data_pin_, 0, 90) != ESP_OK) {
            result = ESP_ERR_TIMEOUT;
            failure_stage = "el sensor nunca bajo la linea (posible falta "
                             "de alimentacion, cableado o pull-up)";
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
        // un pulso en alto cuya duración indica el valor: ~26-28 us para un
        // '0' y ~70 us para un '1'. En vez de medir el ancho completo del
        // pulso (lo que exige ver también su flanco de bajada, y algunos
        // clones del DHT11 no emiten ese flanco tras el bit 40), esperamos
        // el flanco de subida, dejamos pasar 40 us de referencia y miramos
        // en qué nivel quedó la línea: si ya bajó, era un '0'; si sigue en
        // alto, era un '1'.
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

            // Antes del último bit hace falta ver la línea volver a bajo
            // para no confundir el resto del pulso alto actual con el
            // inicio del próximo bit. Tras el bit 40 no hay próximo bit que
            // sincronizar, así que no se exige ese flanco.
            if (bit < 39 &&
                wait_for_level(data_pin_, 0, 200) != ESP_OK) {
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

    /*
     * Algunos clones del DHT11 no emiten un flanco de bajada tras el bit 40
     * (el último de la trama), a diferencia de lo que indica el datasheet
     * oficial. Sin ese flanco, ese bit puntual queda indistinguible entre
     * '0' y '1' por temporización, y en la práctica siempre se observa en
     * alto. Como ese bit es justamente el menos significativo del byte de
     * checksum, se lo excluye de la comparación: los bytes de temperatura y
     * humedad (bytes[0..3]) sí llegan íntegros y siguen validados por el
     * resto del checksum.
     */
    constexpr std::uint8_t UNRELIABLE_LAST_BIT_MASK = 0x01;
    if ((checksum | UNRELIABLE_LAST_BIT_MASK) !=
        (bytes[4] | UNRELIABLE_LAST_BIT_MASK)) {
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

    // El DHT11 solo tiene resolución entera; bytes[1] y bytes[3] son
    // siempre 0 en este modelo (a diferencia del DHT22).
    out_reading.humidity_percent = static_cast<float>(bytes[0]);
    out_reading.temperature_c = static_cast<float>(bytes[2]);
    return ESP_OK;
}
