#pragma once

#include "driver/gpio.h"
#include "esp_err.h"

/**
 * Lectura cruda de un DHT22: humedad relativa y temperatura ambiente.
 */
struct Dht22Reading {
    float temperature_c;
    float humidity_percent;
};

/**
 * Driver mínimo para el sensor DHT22 (AM2302) mediante bit-banging de un
 * solo pin.
 *
 * El DHT22 no usa I2C/SPI: el propio ESP32 debe generar la señal de inicio
 * y luego medir cada pulso para reconstruir los 40 bits que envía el sensor
 * (humedad, temperatura y checksum).
 */
class Dht22Sensor {
public:
    explicit Dht22Sensor(gpio_num_t data_pin);

    /**
     * Realiza una lectura completa. El DHT22 admite como máximo una lectura
     * cada 2 segundos; llamadas más frecuentes suelen fallar o repetir el
     * valor anterior.
     *
     * Devuelve ESP_OK si la lectura y el checksum son válidos.
     */
    esp_err_t read(Dht22Reading &out_reading) const;

private:
    gpio_num_t data_pin_;
};
