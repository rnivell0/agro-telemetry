# Local secrets

The files in this directory contain local credentials and are ignored by Git.
They are mounted at runtime through Docker Compose secrets and are never copied
into an image.

## InfluxDB

- `influxdb-username.txt`: initial administrative username.
- `influxdb-password.txt`: initial administrative password.
- `influxdb-token.txt`: operator token used by InfluxDB initialization and the
  gateway writer.
- `grafana-influxdb-token.txt`: read-only token scoped to the `telemetry`
  bucket and used only by Grafana.

## MQTT

- `mqtt-gateway-password.txt`: password of the `agro-gateway` Mosquitto user,
  mounted into the gateway.
- `mqtt-node-password.txt`: password of the `agro-node` fleet user shared by
  every ESP32. No container mounts it: it is the source for adding the user
  to `mosquitto/config/passwd` and for the password set in the firmware's
  `menuconfig`.

## Grafana

- `grafana-admin-password.txt`: password for the local Grafana `admin` user.

Each file contains only its value, without a trailing newline. Never commit,
print, or copy these values into Compose, provisioning YAML, dashboard JSON, or
container images.
