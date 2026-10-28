# Agro Telemetry

An end-to-end agricultural telemetry pipeline, built to learn embedded and IoT engineering properly: an ESP32 running FreeRTOS with C++20 firmware reads a temperature/humidity sensor and publishes over MQTT with TLS, a C++17 gateway validates each reading and stores it in InfluxDB, and Grafana shows it on a provisioned dashboard.

```mermaid
flowchart LR
    S[DHT22 sensor] -->|single-wire| E[ESP32<br/>FreeRTOS · C++20]
    E -->|I2C| O[SSD1306 OLED]
    E -->|MQTT over TLS<br/>QoS 1| M[Mosquitto]
    M -->|MQTT over TLS| G[Gateway<br/>C++17]
    G -->|HTTP line protocol| I[InfluxDB]
    I --> F[Grafana]
```

Everything to the right of the ESP32 runs on one machine with Docker Compose. The status is honest: this is a bench prototype with three nodes powered over USB, not a field deployment.

> Code comments, log messages and commit messages are in Spanish; this README is in English.

## Repository layout

| Folder | What it is |
|---|---|
| [`firmware/`](firmware/) | ESP-IDF 6 project for an ESP32 DevKit V1: FreeRTOS tasks in C++20 |
| [`gateway/`](gateway/) | C++17 service: MQTT subscriber → validation → InfluxDB writer, with unit tests |
| [`infrastructure/`](infrastructure/) | Docker Compose stack: Mosquitto, InfluxDB, gateway, Grafana |

## How it works

### Firmware (ESP32, FreeRTOS, C++20)

Three application tasks, all at priority 5 on a dual-core ESP32, communicating only through FreeRTOS queues that copy data by value — no shared memory, no mutexes:

- **Identity from the MAC address.** Every board runs the same binary. At boot it reads its factory MAC from eFuse and names itself `esp32-` plus the last three bytes (for example `esp32-1e1574`). That ID is its MQTT client ID, part of its topic and the `device_id` in each payload, so a new board shows up in Grafana with no per-board configuration.
- **`SensorTask`** reads the DHT22 every 2 s with `vTaskDelayUntil()` so the period does not drift, and pushes each reading into a 5-slot queue. If the queue is full it drops the new sample and logs it, rather than blocking the sampling loop.
- **`TelemetryTask`** takes readings from that queue, logs them, hands them to the MQTT client and draws them on the OLED.
- **`MqttClient`** owns its own task, which is the *only* code that touches the ESP-MQTT client and its connection state. Network events (from the ESP-IDF event loop), MQTT events (from ESP-MQTT's internal task) and publish requests all arrive as messages on one queue and are processed in order, so no decision — such as starting the transport — can be taken from two tasks at once.

Health and robustness:

- **Task watchdog:** `SensorTask`, `TelemetryTask` and the MQTT task subscribe to the ESP-IDF task watchdog. If one stops feeding it for 5 s, the board prints a backtrace and reboots. A `static_assert` in each task requires its period or queue wait to be under half the timeout, so changing a period breaks the build, not the board.
- **`TaskMonitor`** logs every task in the system (including the ones ESP-IDF creates) every 30 s, sorted by minimum free stack, plus free heap. Stack sizes are backed by measurements from it.
- **DHT22 driver** is bit-banged on one GPIO in open-drain mode, so the line can be released and read without reconfiguring the pin in the timing-critical window. Full checksum validation; temperature decoded as sign-and-magnitude.
- **TLS:** the broker's CA certificate is embedded in the binary; the client verifies the broker and authenticates with username and password. Publishing never blocks the caller while disconnected.

### Gateway (C++17)

Two threads and a bounded queue:

- The MQTT thread (Paho C++, TLS) subscribes to `agro/devices/+/telemetry` with QoS 1, parses and validates each JSON message and enqueues it. It never touches InfluxDB. The node's identity comes from the topic, which the broker's ACL guarantees, not from the payload: a message whose `device_id` does not match its topic is rejected.
- A persistence thread drains the queue in batches, serializes them to InfluxDB line protocol and writes them over HTTP, with exponential backoff on 429/5xx/network errors. Permanent errors (400/401/403) are logged and dropped instead of retried forever.
- The queue holds 1000 readings. When full it drops the **oldest** reading instead of blocking the MQTT thread.
- Unit tests for the parser, the queue and the serializer run inside the multi-stage Docker build: a failing test means no image.

### Infrastructure (Docker Compose)

- **Mosquitto** on port 8883, TLS only, no anonymous access. An ACL denies everything not listed: each node may only publish to the topic carrying its own client ID (`pattern write agro/devices/%c/telemetry`), and the gateway may only read telemetry.
- **InfluxDB 2** with organization `agro-telemetry`, bucket `telemetry`, 720 h retention.
- **Grafana** with the data source and dashboard provisioned from files; it reads with a token scoped read-only to the telemetry bucket. The dashboard discovers nodes from InfluxDB, shows all of them by default with one line per node plus a per-node status table (latest values, Wi-Fi signal and time since the last message) and a Wi-Fi signal graph, and can show a friendly name per node: edit the hidden `aliases` variable in `infrastructure/grafana/dashboards/agro-telemetry.json` (`esp32-1e1574=Desk,esp32-5de31c=Window`), and Grafana reloads it within 10 s.
- **Secrets** are individual files mounted as Docker secrets under `/run/secrets/` — never plain environment variables, never baked into an image. Private keys and the Mosquitto password file are git-ignored.

### C++ standards

Each part declares its standard explicitly in its build, instead of inheriting a toolchain default:

| Part | Standard | Why |
|---|---|---|
| Firmware | C++20 (`gnu++20`) | The lowest standard the code needs: it uses designated initializers. ESP-IDF 6 would default to `gnu++26`, which would make the build depend on the installed ESP-IDF version. |
| Gateway | C++17 | Nothing in the code needs more. |

A part moves to a newer standard when a feature pays for itself, not to look modern. The candidates already identified are `std::expected` (C++23), so drivers return a value or an error instead of an error code plus an output parameter, and `std::jthread` with `std::stop_token` (C++20), for the gateway's persistence thread shutdown. `std::format` and exceptions stay out of the firmware because of their code size.

### Data contract

The same value changes name between layers on purpose, following each layer's conventions:

| Layer | Humidity field |
|---|---|
| Firmware queue (C++ struct) | `humidity_percent` |
| MQTT payload (JSON) | `humidity_pct` |
| Gateway (C++ struct) | `humidityPct` |
| InfluxDB (line protocol field) | `humidity_pct` |

Example payload on `agro/devices/esp32-1e1574/telemetry`:

```json
{"device_id":"esp32-1e1574","sequence":14,"temperature_c":24.5,"humidity_pct":45.7,"uptime_ms":29367,"rssi_dbm":-61}
```

`rssi_dbm` is the Wi-Fi signal the node sees when it publishes. It is read by the MQTT task at send time, not by the sensor task, because it describes the network rather than the measurement. It is optional, so nodes on older firmware are still accepted, and Grafana graphs it per node to tell a weak signal apart from a power problem.

The gateway rejects a message unless `device_id` is non-empty, `temperature_c` is finite, `humidity_pct` is within 0–100 and, when present, `rssi_dbm` is an integer from −127 to −1. Sequence gaps are logged as warnings, since a lost packet or a reboot explains them.

## Hardware

| Module | Module pin | ESP32 DevKit V1 pin |
|---|---|---|
| DHT22 (3-pin module with pull-up) | VCC / DATA / GND | 3V3 / D4 / GND |
| SSD1306 OLED 128×64 (I2C, 0x3C) | VCC / GND / SCL / SDA | 3V3 / GND / D22 / D21 |

Power the DHT22 from 3.3 V: the module's pull-up goes to VCC and the ESP32 pins are not 5 V tolerant. Mount it away from the board, which heats it.

## Running it

Prerequisites: Docker with Compose, [ESP-IDF v6.0.2](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/index.html), OpenSSL.

**1. Certificates.** The certificates in the repository were issued for my LAN, so generate your own in `infrastructure/mosquitto/certs/`. The server certificate needs a SAN for `mosquitto` (used by the gateway inside Docker) and for the LAN IP of the machine running the broker (used by the ESP32):

```sh
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
  -keyout ca.key -out ca.crt -subj "/CN=Agro Telemetry Local CA"
openssl req -newkey rsa:2048 -nodes -keyout server.key -out server.csr -subj "/CN=mosquitto"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial -days 825 \
  -out server.crt -extfile <(printf "subjectAltName=DNS:mosquitto,DNS:localhost,IP:127.0.0.1,IP:<your-LAN-IP>")
```

Copy `ca.crt` to `firmware/main/certs/mqtt_ca.pem`.

**2. MQTT users.** Create `infrastructure/mosquitto/config/passwd` with two users, `agro-node` and `agro-gateway`, using `mosquitto_passwd`. `agro-node` is the fleet user every node logs in with: nodes are told apart by their client ID, and the ACL in `infrastructure/mosquitto/config/acl` keeps each one on its own topic. The container's `mosquitto` user must be able to read the file. If it can't, Mosquitto exits and the reason appears only in `infrastructure/mosquitto/log/mosquitto.log`, not in `docker logs`.

**3. Secrets.** Create the files listed in [`infrastructure/secrets/README.md`](infrastructure/secrets/README.md), plus `mqtt-gateway-password.txt` with the gateway's MQTT password.

**4. Stack.**

```sh
cd infrastructure
docker compose up -d --build
docker compose logs -f gateway
```

Grafana is at http://localhost:3000.

**5. Firmware.** See [`firmware/README.md`](firmware/README.md). In short: set the Wi-Fi network, broker URI and the shared MQTT user and password in `idf.py menuconfig` under *Agro Telemetry*, then run `idf.py -p <PORT> flash monitor` on each board. The same build works for every node.

**Gateway tests** without Docker:

```sh
cd gateway
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

## Known limitations

- Three nodes, on the bench, powered over USB.
- One node has gone silent twice, for 2.5 h and 10 min, without rebooting and without reaching the broker, and came back only after a power cycle; the reset button did not help. The other two nodes, on the same firmware, had no gaps. Not yet diagnosed: the power supply and the Wi-Fi signal where it is placed are the main suspects.
- All nodes share one MQTT user and password. The ACL keeps each client ID on its own topic, but anyone holding the shared password can connect with another node's client ID and publish as that node. Per-node client certificates (mutual TLS) would close this.
- Server-side TLS only: the broker is verified, clients authenticate with passwords, not certificates.
- Readings taken while the broker is unreachable are dropped, not buffered. Only messages that ESP-MQTT has already accepted are retried under QoS 1.
- InfluxDB keeps 30 days of data.

## What's next

Planned, not built: forwarding telemetry from the gateway to AWS IoT Core with X.509 mutual TLS, a Device Shadow and a rule into DynamoDB or S3. Also planned: a second I2C sensor on the bus the OLED already uses.
