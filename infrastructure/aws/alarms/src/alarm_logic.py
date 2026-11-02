"""Decide si una lectura cambia el estado de alarma de un nodo.

No depende de AWS: handler.py lee el estado guardado, pregunta acá y
guarda el cambio. Así la lógica se prueba en la PC, sin boto3.
"""

from dataclasses import dataclass

OK = "OK"
ALARM = "ALARM"


@dataclass(frozen=True)
class Threshold:
    metric: str  # campo del payload, por ejemplo humidity_pct
    label: str  # para el mail; ASCII, porque va en el asunto
    unit: str
    alarm_above: float
    clear_below: float

    def __post_init__(self):
        # Con clear_below por encima de alarm_above, una lectura entre los
        # dos entra y sale de alarma cada vez: un mail por lectura. Mejor
        # que la Lambda no arranque, con el motivo en su log.
        if self.clear_below > self.alarm_above:
            raise ValueError(
                f"{self.metric}: clear_below ({self.clear_below}) no puede "
                f"superar alarm_above ({self.alarm_above})"
            )


def next_state(current: str | None, value: float, threshold: Threshold) -> str | None:
    """Devuelve el estado nuevo, o None si la lectura no lo cambia.

    Histéresis: entra en alarma por encima de alarm_above y sale recién por
    debajo de clear_below. Entre los dos queda como estaba, para que un
    valor que oscila alrededor del umbral no mande un mail por lectura.
    Sin estado guardado (None), el nodo cuenta como OK.
    """
    if value > threshold.alarm_above and current != ALARM:
        return ALARM
    if value < threshold.clear_below and current == ALARM:
        return OK
    return None


def is_reading(value: object) -> bool:
    """True si el valor sirve como lectura: un número, pero no un booleano,
    que en Python también es un int."""
    return isinstance(value, (int, float)) and not isinstance(value, bool)
