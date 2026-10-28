"""Publica un firmware de AWS como Job de OTA para una placa.

Sube el binario al bucket del stack agro-firmware-updates y crea un Job de
IoT para el thing. El nodo lo recibe por MQTT, descarga el binario con una
URL firmada que arma AWS al entregarle el Job, verifica versión y SHA-256
y reinicia en la imagen nueva (ver OtaUpdater).

Uso, con la CLI de AWS configurada con agro-iot-cli:

    python tools/ota_release.py build-aws/agroTelemetryFirmware.bin \\
        --thing esp32-5de31c --bucket <BucketName> --role-arn <SigningRoleArn>

BucketName y SigningRoleArn son las salidas del stack agro-firmware-updates.
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REGION = "us-east-2"
# Cabecera de la imagen (24 bytes) + cabecera del primer segmento (8):
# ahí empieza esp_app_desc_t.
APP_DESC_OFFSET = 32
APP_DESC_MAGIC = 0xABCD5432
# esp_app_desc_t: magic (4), secure_version (4), reservado (8), version (32).
VERSION_OFFSET = APP_DESC_OFFSET + 16
VERSION_SIZE = 32
# Byte 23 de la cabecera: si el build agregó el SHA-256 al final.
HASH_APPENDED_OFFSET = 23
# Lo que tarda en vencer la URL firmada desde que AWS entrega el Job.
URL_EXPIRATION_S = 3600
# Si el nodo no lo termina en este tiempo, AWS lo marca TIMED_OUT.
IN_PROGRESS_TIMEOUT_MIN = 15


def aws(*args: str) -> str:
    executable = shutil.which("aws")
    if executable is None:
        sys.exit("No se encontró la CLI de AWS en el PATH")
    result = subprocess.run([executable, *args, "--region", REGION],
                            check=True, capture_output=True, text=True)
    return result.stdout.strip()


def read_image(path: Path) -> tuple[str, str]:
    """Devuelve la versión y el SHA-256 que el nodo va a comparar."""
    data = path.read_bytes()
    (magic,) = struct.unpack_from("<I", data, APP_DESC_OFFSET)
    if magic != APP_DESC_MAGIC:
        sys.exit(f"{path} no parece una imagen de aplicación de ESP-IDF")
    if data[HASH_APPENDED_OFFSET] != 1:
        sys.exit("La imagen no trae el SHA-256 al final; el nodo no podría verificarla")

    version = data[VERSION_OFFSET:VERSION_OFFSET + VERSION_SIZE].split(b"\0")[0].decode()
    appended = data[-32:]
    if hashlib.sha256(data[:-32]).digest() != appended:
        sys.exit("El SHA-256 al final de la imagen no coincide: el .bin está dañado")
    return version, appended.hex()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("image", type=Path, help="el .bin de la build de AWS")
    parser.add_argument("--thing", required=True, help="thing de destino, p. ej. esp32-5de31c")
    parser.add_argument("--bucket", required=True, help="BucketName del stack")
    parser.add_argument("--role-arn", required=True, help="SigningRoleArn del stack")
    args = parser.parse_args()

    version, sha256 = read_image(args.image)
    if version.endswith("-dirty"):
        print(f"Aviso: {version} se compiló con cambios sin commitear")

    # El jobId termina en un topic MQTT: solo [a-zA-Z0-9_-].
    job_id = re.sub(r"[^A-Za-z0-9_-]", "-", f"ota-{version}-{int(time.time())}")
    key = f"firmware/{version}.bin"

    aws("s3", "cp", str(args.image), f"s3://{args.bucket}/{key}")
    print(f"Subido s3://{args.bucket}/{key}")

    account = aws("sts", "get-caller-identity", "--query", "Account", "--output", "text")
    document = {
        "operation": "ota",
        "version": version,
        "sha256": sha256,
        # AWS reemplaza esto por una URL firmada con el rol de firma al
        # entregarle el Job al nodo: el documento guardado no la contiene.
        "url": f"${{aws:iot:s3-presigned-url:https://s3.amazonaws.com/{args.bucket}/{key}}}",
    }

    with tempfile.TemporaryDirectory() as work_dir:
        document_path = os.path.join(work_dir, "job.json")
        Path(document_path).write_text(json.dumps(document), encoding="utf-8")
        aws("iot", "create-job",
            "--job-id", job_id,
            "--targets", f"arn:aws:iot:{REGION}:{account}:thing/{args.thing}",
            "--document", f"file://{document_path}",
            "--presigned-url-config",
            f"roleArn={args.role_arn},expiresInSec={URL_EXPIRATION_S}",
            "--timeout-config", f"inProgressTimeoutInMinutes={IN_PROGRESS_TIMEOUT_MIN}",
            "--target-selection", "SNAPSHOT")

    print(f"Job {job_id} creado: {args.thing} -> {version}")
    print(f"Estado: aws iot describe-job-execution --job-id {job_id} "
          f"--thing-name {args.thing} --region {REGION}")


if __name__ == "__main__":
    main()
