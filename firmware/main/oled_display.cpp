#include "oled_display.hpp"

#include <cstring>

#include "esp_log.h"

namespace {
constexpr char TAG[] = "OLED";
constexpr int I2C_TIMEOUT_MS = 1000;

/**
 * Secuencia de inicialización estándar para un SSD1306 de 128x64 con bomba
 * de carga interna, tal como la usan la gran mayoría de los módulos
 * económicos de 0.96"/0.91". Cada valor es un comando enviado tal cual.
 */
/**
 * Fuente propia de 3x5 píxeles, minúscula a propósito para no depender de
 * ninguna tabla de fuentes externa. Cada fila es un valor de 3 bits: el bit
 * más significativo (0b100) es la columna izquierda del glifo.
 *
 * Solo cubre los caracteres que usan las etiquetas de esta pantalla
 * (mayúsculas, dígitos, espacio, '.', ':' y '%'); cualquier otro caracter
 * se dibuja en blanco.
 */
struct Glyph {
    char character;
    std::uint8_t rows[5];
};

constexpr Glyph FONT[] = {
    {'0', {0b111, 0b101, 0b101, 0b101, 0b111}},
    {'1', {0b010, 0b110, 0b010, 0b010, 0b111}},
    {'2', {0b111, 0b001, 0b111, 0b100, 0b111}},
    {'3', {0b111, 0b001, 0b111, 0b001, 0b111}},
    {'4', {0b101, 0b101, 0b111, 0b001, 0b001}},
    {'5', {0b111, 0b100, 0b111, 0b001, 0b111}},
    {'6', {0b111, 0b100, 0b111, 0b101, 0b111}},
    {'7', {0b111, 0b001, 0b001, 0b001, 0b001}},
    {'8', {0b111, 0b101, 0b111, 0b101, 0b111}},
    {'9', {0b111, 0b101, 0b111, 0b001, 0b111}},
    {'.', {0b000, 0b000, 0b000, 0b000, 0b010}},
    {':', {0b010, 0b000, 0b010, 0b000, 0b000}},
    {'%', {0b101, 0b001, 0b010, 0b100, 0b101}},
    {'A', {0b111, 0b101, 0b111, 0b101, 0b101}},
    {'C', {0b011, 0b100, 0b100, 0b100, 0b011}},
    {'E', {0b111, 0b100, 0b111, 0b100, 0b111}},
    {'H', {0b101, 0b101, 0b111, 0b101, 0b101}},
    {'K', {0b101, 0b110, 0b100, 0b110, 0b101}},
    {'M', {0b101, 0b111, 0b101, 0b101, 0b101}},
    {'N', {0b101, 0b111, 0b101, 0b101, 0b101}},
    {'O', {0b111, 0b101, 0b101, 0b101, 0b111}},
    {'P', {0b111, 0b101, 0b111, 0b100, 0b100}},
    {'Q', {0b111, 0b101, 0b101, 0b111, 0b001}},
    {'S', {0b111, 0b100, 0b111, 0b001, 0b111}},
    {'T', {0b111, 0b010, 0b010, 0b010, 0b010}},
    {'U', {0b101, 0b101, 0b101, 0b101, 0b111}},
    {' ', {0b000, 0b000, 0b000, 0b000, 0b000}},
};

const std::uint8_t *find_glyph(char character)
{
    for (const Glyph &glyph : FONT) {
        if (glyph.character == character) {
            return glyph.rows;
        }
    }
    return nullptr;
}

constexpr int GLYPH_WIDTH = 3;
constexpr int GLYPH_HEIGHT = 5;
constexpr int GLYPH_SPACING = 1;

constexpr std::uint8_t INIT_SEQUENCE[] = {
    0xAE,       // Display OFF
    0xD5, 0x80, // Divisor de reloj / frecuencia de refresco
    0xA8, 0x3F, // Multiplex ratio = 64 (para pantallas de 64 filas)
    0xD3, 0x00, // Sin desplazamiento vertical
    0x40,       // Linea de inicio de RAM = 0
    0x8D, 0x14, // Habilitar la bomba de carga interna (necesaria sin V+ externo)
    0x20, 0x00, // Modo de direccionamiento horizontal
    0xA1,       // Remapeo de segmentos (orientacion horizontal correcta)
    0xC8,       // Direccion de escaneo COM invertida (orientacion vertical correcta)
    0xDA, 0x12, // Configuracion de pines COM
    0x81, 0xCF, // Contraste
    0xD9, 0xF1, // Periodo de precarga
    0xDB, 0x40, // Nivel de deseleccion VCOMH
    0xA4,       // Mostrar el contenido real de la RAM (no forzar todo encendido)
    0xA6,       // Modo normal, no invertido
    0xAF,       // Display ON
};

} // namespace

OledDisplay::OledDisplay(gpio_num_t sda_pin,
                          gpio_num_t scl_pin,
                          std::uint16_t i2c_address)
    : sda_pin_{sda_pin},
      scl_pin_{scl_pin},
      i2c_address_{i2c_address}
{
}

OledDisplay::~OledDisplay()
{
    if (device_ != nullptr) {
        i2c_master_bus_rm_device(device_);
    }
    if (bus_ != nullptr) {
        i2c_del_master_bus(bus_);
    }
}

esp_err_t OledDisplay::init()
{
    const i2c_master_bus_config_t bus_config = {
        .i2c_port = -1,
        .sda_io_num = sda_pin_,
        .scl_io_num = scl_pin_,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .intr_priority = 0,
        .trans_queue_depth = 0,
        .flags =
            {
                .enable_internal_pullup = 1,
                .allow_pd = 0,
            },
    };

    esp_err_t result = i2c_new_master_bus(&bus_config, &bus_);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "No se pudo crear el bus I2C: %s",
                 esp_err_to_name(result));
        return result;
    }

    result = i2c_master_probe(bus_, i2c_address_, I2C_TIMEOUT_MS);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "No se detecto ningun dispositivo I2C en la direccion "
                 "0x%02X: %s",
                 i2c_address_,
                 esp_err_to_name(result));
        scan_bus_and_log();
        return result;
    }

    const i2c_device_config_t device_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = i2c_address_,
        .scl_speed_hz = 400000,
        .scl_wait_us = 0,
        .flags =
            {
                .disable_ack_check = 0,
            },
    };

    result = i2c_master_bus_add_device(bus_, &device_config, &device_);
    if (result != ESP_OK) {
        ESP_LOGE(TAG,
                 "No se pudo agregar el dispositivo I2C: %s",
                 esp_err_to_name(result));
        return result;
    }

    for (const std::uint8_t command : INIT_SEQUENCE) {
        result = send_command(command);
        if (result != ESP_OK) {
            ESP_LOGE(TAG,
                     "Fallo la secuencia de inicializacion del SSD1306: %s",
                     esp_err_to_name(result));
            return result;
        }
    }

    ESP_LOGI(TAG, "Pantalla OLED inicializada en 0x%02X", i2c_address_);
    ready_ = true;
    return clear();
}

esp_err_t OledDisplay::send_command(std::uint8_t command)
{
    // Byte de control 0x00: Co=0 y D/C#=0 indican "esto es un comando", no
    // datos de pixeles.
    const std::uint8_t payload[2] = {0x00, command};
    return i2c_master_transmit(
        device_, payload, sizeof(payload), I2C_TIMEOUT_MS);
}

esp_err_t OledDisplay::set_all_pixels(std::uint8_t pattern)
{
    std::memset(framebuffer_, pattern, sizeof(framebuffer_));
    return push_framebuffer();
}

esp_err_t OledDisplay::clear()
{
    return set_all_pixels(0x00);
}

esp_err_t OledDisplay::fill()
{
    return set_all_pixels(0xFF);
}

void OledDisplay::scan_bus_and_log() const
{
    // Direcciones I2C reservadas fuera de 0x03-0x77: se evita sondearlas
    // porque algunas activan comandos especiales del bus en vez de
    // responder como un dispositivo normal.
    constexpr int SCAN_TIMEOUT_MS = 50;
    bool found_any = false;

    ESP_LOGW(TAG, "Escaneando el bus I2C en busca de dispositivos...");
    for (std::uint16_t address = 0x03; address <= 0x77; ++address) {
        if (i2c_master_probe(bus_, address, SCAN_TIMEOUT_MS) == ESP_OK) {
            ESP_LOGW(TAG, "  Dispositivo encontrado en 0x%02X", address);
            found_any = true;
        }
    }

    if (!found_any) {
        ESP_LOGW(TAG,
                 "No respondio ningun dispositivo en todo el bus I2C. Esto "
                 "apunta a un problema de cableado o alimentacion (SDA/SCL "
                 "invertidos, GND/VCC flojos, o un pin distinto a D21/D22), "
                 "no a una direccion equivocada.");
    }
}

esp_err_t OledDisplay::push_framebuffer()
{
    // Antes de escribir los pixeles, se fija el rango completo de columnas
    // y paginas para que el SSD1306 vuelque el framebuffer en orden.
    esp_err_t result = send_command(0x21); // Rango de columnas
    if (result != ESP_OK) {
        return result;
    }
    result = send_command(0);
    if (result != ESP_OK) {
        return result;
    }
    result = send_command(WIDTH - 1);
    if (result != ESP_OK) {
        return result;
    }

    result = send_command(0x22); // Rango de paginas
    if (result != ESP_OK) {
        return result;
    }
    result = send_command(0);
    if (result != ESP_OK) {
        return result;
    }
    result = send_command((HEIGHT / 8) - 1);
    if (result != ESP_OK) {
        return result;
    }

    transmit_buffer_[0] = 0x40; // Byte de control: siguen datos de pixeles.
    std::memcpy(transmit_buffer_ + 1, framebuffer_, FRAMEBUFFER_SIZE);
    return i2c_master_transmit(
        device_, transmit_buffer_, sizeof(transmit_buffer_), I2C_TIMEOUT_MS);
}

void OledDisplay::set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) {
        return;
    }

    // El SSD1306 organiza su RAM en "paginas" de 8 filas: cada byte del
    // framebuffer representa una columna de 8 pixeles verticales dentro de
    // una pagina, con el bit menos significativo arriba.
    const int page = y / 8;
    const std::uint8_t bit = static_cast<std::uint8_t>(1u << (y % 8));
    std::uint8_t &byte = framebuffer_[page * WIDTH + x];

    if (on) {
        byte |= bit;
    } else {
        byte &= static_cast<std::uint8_t>(~bit);
    }
}

void OledDisplay::draw_char(int x, int y, char character, int scale)
{
    const std::uint8_t *rows = find_glyph(character);
    if (rows == nullptr) {
        return; // Caracter no soportado: se deja en blanco.
    }

    for (int row = 0; row < GLYPH_HEIGHT; ++row) {
        for (int col = 0; col < GLYPH_WIDTH; ++col) {
            const bool pixel_on =
                ((rows[row] >> (GLYPH_WIDTH - 1 - col)) & 0x01) != 0;
            if (!pixel_on) {
                continue;
            }

            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    set_pixel(
                        x + col * scale + dx, y + row * scale + dy, true);
                }
            }
        }
    }
}

void OledDisplay::draw_text(int x, int y, const char *text, int scale)
{
    int cursor_x = x;
    for (const char *character = text; *character != '\0'; ++character) {
        draw_char(cursor_x, y, *character, scale);
        cursor_x += (GLYPH_WIDTH + GLYPH_SPACING) * scale;
    }
}

esp_err_t OledDisplay::show_lines(std::initializer_list<const char *> lines)
{
    if (!ready_) {
        return ESP_ERR_INVALID_STATE;
    }

    constexpr int SCALE = 2;
    constexpr int LINE_HEIGHT = GLYPH_HEIGHT * SCALE + 2;

    std::memset(framebuffer_, 0, sizeof(framebuffer_));

    int y = 0;
    for (const char *line : lines) {
        draw_text(0, y, line, SCALE);
        y += LINE_HEIGHT;
    }

    return push_framebuffer();
}
