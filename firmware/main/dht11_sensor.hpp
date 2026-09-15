#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

/**
 * Lectura cruda de un DHT11: humedad relativa y temperatura ambiente.
 */
struct Dht11Reading {
    float temperature_c;
    float humidity_percent;
};

/**
 * Driver mínimo para el sensor DHT11 mediante bit-banging de un solo pin.
 *
 * El DHT11 no usa I2C/SPI: el propio ESP32 debe generar la señal de inicio
 * y luego medir el ancho de cada pulso para reconstruir los 40 bits que
 * envía el sensor (humedad, temperatura y checksum).
 */
class Dht11Sensor {
public:
    explicit Dht11Sensor(gpio_num_t data_pin);

    /**
     * Realiza una lectura completa. El DHT11 solo admite una lectura por
     * segundo como máximo; llamadas más frecuentes suelen fallar.
     *
     * Devuelve ESP_OK si la lectura y el checksum son válidos.
     */
    esp_err_t read(Dht11Reading &out_reading) const;

private:
    gpio_num_t data_pin_;
};
