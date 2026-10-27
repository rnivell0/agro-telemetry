# Firmware — ESP32 · FreeRTOS · C++20

ESP-IDF 6.0.2 project for an ESP32 DevKit V1 (30 pins). It reads a DHT22 every 2 s, shows the values on an SSD1306 OLED and publishes them over MQTT with TLS. See the [root README](../README.md) for the whole pipeline.

## Build, configure, flash

From this folder, with the ESP-IDF environment active:

```sh
idf.py menuconfig          # Agro Telemetry → Wi-Fi SSID/password, broker URI, MQTT user/password, topic
idf.py build
idf.py -p <PORT> flash monitor
```

- The broker URI must be the LAN address of the machine running Mosquitto (`mqtts://<ip>:8883`), not `localhost`.
- `main/certs/mqtt_ca.pem` must be the CA that signed the broker's certificate. It is embedded in the binary.
- The code is C++20 (`gnu++20`), set in `main/CMakeLists.txt` so the build does not depend on the default of the installed ESP-IDF.
- `sdkconfig` is git-ignored because menuconfig stores the Wi-Fi and MQTT credentials in it in plain text. A fresh clone regenerates it from `sdkconfig.defaults` plus the placeholders in `main/Kconfig.projbuild`.
- After changing `sdkconfig.defaults`, check that the value actually reached `sdkconfig`, because an existing `sdkconfig` does not always pick up new defaults.

## Tasks

| Task | Priority | Stack | Min. free (measured) | Job |
|---|---|---|---|---|
| `SensorTask` | 5 | 2048 B | ~860–990 B | Reads the DHT22 every 2 s and queues the reading |
| `TelemetryTask` | 5 | 3072 B | ~1790 B | Logs, publishes and draws each reading |
| `MqttClient` | 5 | 3072 B | ~1620 B | Sole owner of the ESP-MQTT client and its state |
| `TaskMonitor` | 1 | 3072 B | ~2000–2600 B | Logs all tasks, their minimum free stack and the heap every 30 s |

`SensorTask` → queue (5 readings) → `TelemetryTask` → message queue (10) → `MqttClient` task → ESP-MQTT outbox → broker.

The first three tasks are subscribed to the task watchdog (5 s, panic and reboot). The minimum-free figures come from `TaskMonitor` on the real board, with every code path exercised: a reading that never reaches `TelemetryTask` makes it look far lighter than it is.

## Source map

| File | Responsibility |
|---|---|
| `main.cpp` | Creates everything in order. The OLED and `TaskMonitor` are optional: if they fail, the rest still starts. |
| `sensor_task.*` | Periodic sampling with `vTaskDelayUntil()` |
| `telemetry_task.*` | Fan-out of each reading to log, MQTT and OLED |
| `mqtt_client.*` | Single-owner MQTT task: TLS, user/password, QoS 1, never blocks callers |
| `wifi_manager.*` | Station mode with automatic reconnection; "connected" means an IP address was obtained |
| `dht22_sensor.*` | Bit-banged single-wire driver, open-drain pin, full checksum |
| `oled_display.*` | SSD1306 over the ESP-IDF 6 `i2c_master` driver, with a custom 3×5 font and a bus scan for diagnostics |
| `task_monitor.*` | `uxTaskGetSystemState()` report. Needs `CONFIG_FREERTOS_USE_TRACE_FACILITY`. |
| `telemetry_data.hpp` | The plain struct copied through the sensor queue |

## Wiring

| Module | Pin | Board pin |
|---|---|---|
| DHT22 | VCC / DATA / GND | 3V3 / D4 / GND |
| OLED SSD1306 | VCC / GND / SCL / SDA | 3V3 / GND / D22 / D21 |

On this board the "D" number printed on the silkscreen matches the real GPIO number. That is not true of every ESP32 board.

## Lessons from the bench

- **SDA and SCL swapped means total silence on I2C.** A full scan from 0x03 to 0x77 that finds nothing points to wiring, not to a wrong address.
- **The DHT22 answers faster than the DHT11 did.** Reconfiguring the pin between the start signal and the sensor's response made the driver miss that response, and every frame came out shifted by one bit. The checksum failed, but each frame was valid once shifted back by one position. Open-drain mode removed the reconfiguration.
- **A sensor taped to the board reads too hot,** because the regulator and the Wi-Fi radio warm it.
- **With a blocked task, the watchdog backtrace shows the IDLE task,** because the watchdog prints whatever was running at that moment. The guilty task is named in the "did not reset the watchdog in time" line.
