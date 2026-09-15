#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_err.h"

/**
 * Driver mínimo para una pantalla OLED SSD1306 de 128x64 por I2C.
 *
 * Este primer paso solo permite encender o apagar todos los píxeles, para
 * confirmar que el cableado, la dirección I2C y la secuencia de
 * inicialización son correctos antes de agregar renderizado de texto.
 */
class OledDisplay {
public:
    explicit OledDisplay(gpio_num_t sda_pin,
                          gpio_num_t scl_pin,
                          std::uint16_t i2c_address = 0x3C);
    ~OledDisplay();

    OledDisplay(const OledDisplay &) = delete;
    OledDisplay &operator=(const OledDisplay &) = delete;

    /**
     * Crea el bus I2C, confirma que el dispositivo responde en la dirección
     * configurada y le envía la secuencia de inicialización del SSD1306.
     */
    esp_err_t init();

    /** Apaga todos los píxeles. */
    esp_err_t clear();

    /** Enciende todos los píxeles (útil como prueba visual de cableado). */
    esp_err_t fill();

    /**
     * Dibuja una a cuatro líneas de texto, una debajo de otra (fuente
     * propia de 3x5 píxeles, en mayúsculas), y las envía a la pantalla. Si
     * `init()` no encontró la pantalla, no hace nada y devuelve
     * ESP_ERR_INVALID_STATE.
     *
     * Los caracteres soportados son: A-Z, 0-9, espacio, '.', ':' y '%'.
     * Cualquier otro caracter se dibuja en blanco.
     */
    esp_err_t show_lines(std::initializer_list<const char *> lines);

    bool is_ready() const
    {
        return ready_;
    }

private:
    static constexpr int WIDTH = 128;
    static constexpr int HEIGHT = 64;
    static constexpr std::size_t FRAMEBUFFER_SIZE =
        static_cast<std::size_t>(WIDTH) * static_cast<std::size_t>(HEIGHT) /
        8;

    esp_err_t send_command(std::uint8_t command);
    esp_err_t set_all_pixels(std::uint8_t pattern);
    esp_err_t push_framebuffer();
    void scan_bus_and_log() const;

    void set_pixel(int x, int y, bool on);
    void draw_char(int x, int y, char character, int scale);
    void draw_text(int x, int y, const char *text, int scale);

    gpio_num_t sda_pin_;
    gpio_num_t scl_pin_;
    std::uint16_t i2c_address_;
    i2c_master_bus_handle_t bus_{nullptr};
    i2c_master_dev_handle_t device_{nullptr};
    bool ready_{false};

    std::uint8_t framebuffer_[FRAMEBUFFER_SIZE]{};
    // Buffer de transmisión: primer byte = byte de control I2C (0x40 indica
    // "lo que sigue son datos de píxeles"), seguido de todo el framebuffer.
    // Se guarda como miembro para no reservar más de 1 KB en la pila de la
    // tarea que llame a estos métodos.
    std::uint8_t transmit_buffer_[FRAMEBUFFER_SIZE + 1]{};
};
