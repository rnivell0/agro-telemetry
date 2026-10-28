"""Graba la identidad AWS IoT de una placa en su partición certs.

Cada placa lleva su propio certificado y clave privada; el firmware de AWS
es el mismo para todas y los lee de ahí al arrancar (ver MqttClient).

Uso, con el entorno de ESP-IDF activo y la placa ya flasheada con la tabla
de particiones que incluye certs:

    python tools/aws_identity.py <carpeta> --port COM3

<carpeta> tiene device.crt y device.key, los que generó
`aws iot create-keys-and-certificate` para el thing de esa placa.
"""

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path

PARTITION_NAME = "certs"
PARTITION_SIZE = "0x6000"  # igual que en partitions.csv
NAMESPACE = "aws_iot"
# Los buffers de MqttClient son de 2048 bytes, contando el '\0' final.
MAX_PEM_BYTES = 2047


def idf_tool(*parts: str) -> str:
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        sys.exit("IDF_PATH no está definido: activar el entorno de ESP-IDF")
    return str(Path(idf_path, "components", *parts))


def check_pem(path: Path) -> None:
    if not path.is_file():
        sys.exit(f"No existe {path}")
    size = path.stat().st_size
    if size > MAX_PEM_BYTES:
        sys.exit(f"{path.name} ocupa {size} bytes; el firmware acepta hasta {MAX_PEM_BYTES}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("device_dir", type=Path, help="carpeta con device.crt y device.key")
    parser.add_argument("--port", required=True, help="puerto serie de la placa, p. ej. COM3")
    args = parser.parse_args()

    certificate = args.device_dir / "device.crt"
    key = args.device_dir / "device.key"
    check_pem(certificate)
    check_pem(key)

    # La imagen lleva la clave privada en claro: se arma en una carpeta
    # temporal que se borra al terminar, nunca dentro del repo.
    with tempfile.TemporaryDirectory() as work_dir:
        csv_path = Path(work_dir, "identity.csv")
        image_path = Path(work_dir, "identity.bin")
        csv_path.write_text(
            "key,type,encoding,value\n"
            f"{NAMESPACE},namespace,,\n"
            f"device_crt,file,string,{certificate.resolve()}\n"
            f"device_key,file,string,{key.resolve()}\n",
            encoding="utf-8",
        )

        subprocess.run(
            [sys.executable,
             idf_tool("nvs_flash", "nvs_partition_generator", "nvs_partition_gen.py"),
             "generate", str(csv_path), str(image_path), PARTITION_SIZE],
            check=True,
        )

        # parttool busca la partición por nombre en la tabla que tiene la
        # placa: si todavía tiene la tabla vieja, falla en vez de escribir
        # sobre otra cosa.
        subprocess.run(
            [sys.executable,
             idf_tool("partition_table", "parttool.py"),
             "--port", args.port,
             "write_partition", "--partition-name", PARTITION_NAME,
             "--input", str(image_path)],
            check=True,
        )

    print(f"Identidad grabada en la partición {PARTITION_NAME} desde {args.device_dir}")


if __name__ == "__main__":
    main()
