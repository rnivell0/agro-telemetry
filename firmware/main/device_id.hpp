#pragma once

#include <array>

#include "esp_err.h"

/**
 * Identidad del nodo: "esp32-" seguido de los últimos 3 bytes del MAC de
 * fábrica en hexadecimal, por ejemplo "esp32-5de31c".
 *
 * El MAC viene grabado en el eFuse de cada chip, así que el mismo binario
 * sirve para todas las placas y cada una se identifica sola. Es el
 * client ID MQTT, va en el topic y en el campo device_id del JSON.
 */
using DeviceId = std::array<char, 13>; // "esp32-" + 6 dígitos hex + '\0'

esp_err_t read_device_id(DeviceId &out);
