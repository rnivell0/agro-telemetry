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

## Grafana

- `grafana-admin-password.txt`: password for the local Grafana `admin` user.

Each file contains only its value, without a trailing newline. Never commit,
print, or copy these values into Compose, provisioning YAML, dashboard JSON, or
container images.
