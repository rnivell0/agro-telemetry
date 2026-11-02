"""Lambda de alarmas por umbral.

La invoca la regla agro_telemetry_to_alarms con cada lectura muestreada.
Cuando un nodo entra o sale de alarma, guarda el evento en agro-events y
avisa por mail con SNS. Una lectura que no cambia nada no escribe nada.

agro-events guarda dos tipos de fila por nodo (device_id):
- sk = "STATE": el estado actual de cada métrica (OK o ALARM).
- sk = "EVENT#<received_at>#<métrica>": un evento por cambio, que
  DynamoDB borra solo al pasar expires_at.
"""

import logging
import os
from decimal import Decimal

import boto3

from alarm_logic import ALARM, Threshold, is_reading, next_state

logger = logging.getLogger()
logger.setLevel(logging.INFO)

# Fuera del handler: se crean una vez por instancia de la Lambda y se
# reutilizan en las invocaciones siguientes. El estado de las alarmas, en
# cambio, no puede vivir acá: AWS recicla las instancias y corre varias a
# la vez, y cada una tendría su propia copia.
TABLE = boto3.resource("dynamodb").Table(os.environ["EVENTS_TABLE"])
SNS = boto3.client("sns")
TOPIC_ARN = os.environ["ALARM_TOPIC_ARN"]
RETENTION_SECONDS = int(os.environ["RETENTION_DAYS"]) * 86400

THRESHOLDS = (
    Threshold(
        metric="temperature_c",
        label="Temperatura",
        unit="°C",
        alarm_above=float(os.environ["TEMPERATURE_ALARM_ABOVE"]),
        clear_below=float(os.environ["TEMPERATURE_CLEAR_BELOW"]),
    ),
    Threshold(
        metric="humidity_pct",
        label="Humedad",
        unit="%",
        alarm_above=float(os.environ["HUMIDITY_ALARM_ABOVE"]),
        clear_below=float(os.environ["HUMIDITY_CLEAR_BELOW"]),
    ),
)

STATE_KEY = "STATE"


def handler(event, context):
    device_id = event["device_id"]
    received_at = int(event["received_at"])

    # Una lectura por invocación, para las dos métricas. Eventualmente
    # consistente alcanza: si está desactualizada, la escritura condicional
    # de abajo falla en vez de pisar el estado.
    state = TABLE.get_item(Key={"device_id": device_id, "sk": STATE_KEY}).get("Item", {})

    for threshold in THRESHOLDS:
        value = event.get(threshold.metric)
        if not is_reading(value):
            logger.warning("%s: %s no es un número: %r", device_id, threshold.metric, value)
            continue

        current = state.get(threshold.metric)
        new = next_state(current, value, threshold)
        if new is None:
            continue

        if claim_transition(device_id, threshold.metric, current, new, received_at):
            record_and_notify(device_id, threshold, new, value, received_at)


def claim_transition(device_id, metric, current, new, received_at):
    """Guarda el estado nuevo solo si el guardado sigue siendo `current`.

    Es control de concurrencia optimista: si dos invocaciones ven el mismo
    cruce de umbral al mismo tiempo, solo una gana la escritura y solo esa
    manda el mail. Devuelve False si otra ya lo hizo.
    """
    if current is None:
        condition = "attribute_not_exists(#state)"
        values = {":new": new, ":at": received_at}
    else:
        condition = "#state = :current"
        values = {":new": new, ":at": received_at, ":current": current}

    try:
        TABLE.update_item(
            Key={"device_id": device_id, "sk": STATE_KEY},
            UpdateExpression="SET #state = :new, #at = :at",
            ConditionExpression=condition,
            ExpressionAttributeNames={"#state": metric, "#at": f"{metric}_changed_at"},
            ExpressionAttributeValues=values,
        )
        return True
    except TABLE.meta.client.exceptions.ConditionalCheckFailedException:
        logger.info("%s: otra invocación ya cambió %s", device_id, metric)
        return False


def record_and_notify(device_id, threshold, new, value, received_at):
    entering = new == ALARM
    limit = threshold.alarm_above if entering else threshold.clear_below

    TABLE.put_item(
        Item={
            "device_id": device_id,
            "sk": f"EVENT#{received_at}#{threshold.metric}",
            "metric": threshold.metric,
            "state": new,
            # boto3 no acepta float en DynamoDB: pide Decimal.
            "value": Decimal(str(value)),
            "threshold": Decimal(str(limit)),
            "received_at": received_at,
            "expires_at": received_at // 1000 + RETENTION_SECONDS,
        }
    )

    if entering:
        subject = f"agro: {threshold.label} alta en {device_id}"
        body = f"{threshold.label} de {device_id}: {value} {threshold.unit}, por encima de {limit} {threshold.unit}."
    else:
        subject = f"agro: {threshold.label} normal en {device_id}"
        body = f"{threshold.label} de {device_id}: {value} {threshold.unit}, de nuevo por debajo de {limit} {threshold.unit}."

    SNS.publish(TopicArn=TOPIC_ARN, Subject=subject, Message=body)
    logger.info("%s: %s pasó a %s (%s)", device_id, threshold.metric, new, value)
