"""Tests de la lógica de alarmas, sin AWS.

    python -m unittest discover -s infrastructure/aws/alarms/tests
"""

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))

from alarm_logic import ALARM, OK, Threshold, is_reading, next_state  # noqa: E402

HUMIDITY = Threshold("humidity_pct", "Humedad", "%", alarm_above=75.0, clear_below=72.0)


class NextStateTest(unittest.TestCase):
    def test_without_saved_state_a_normal_reading_changes_nothing(self):
        self.assertIsNone(next_state(None, 60.0, HUMIDITY))

    def test_without_saved_state_a_high_reading_raises_the_alarm(self):
        self.assertEqual(next_state(None, 80.0, HUMIDITY), ALARM)

    def test_crossing_the_threshold_raises_the_alarm(self):
        self.assertEqual(next_state(OK, 75.1, HUMIDITY), ALARM)

    def test_exactly_at_the_threshold_is_not_an_alarm(self):
        self.assertIsNone(next_state(OK, 75.0, HUMIDITY))

    def test_staying_high_does_not_repeat_the_alarm(self):
        self.assertIsNone(next_state(ALARM, 80.0, HUMIDITY))

    def test_between_the_two_thresholds_the_alarm_stays(self):
        # Histéresis: bajar de 75 no alcanza para salir.
        self.assertIsNone(next_state(ALARM, 73.0, HUMIDITY))

    def test_between_the_two_thresholds_ok_stays_ok(self):
        self.assertIsNone(next_state(OK, 73.0, HUMIDITY))

    def test_dropping_below_clear_ends_the_alarm(self):
        self.assertEqual(next_state(ALARM, 71.9, HUMIDITY), OK)

    def test_low_reading_without_alarm_changes_nothing(self):
        self.assertIsNone(next_state(OK, 50.0, HUMIDITY))

    def test_oscillating_around_the_threshold_alarms_once(self):
        state = OK
        changes = []
        for value in (74.0, 76.0, 74.5, 75.5, 74.0, 76.0, 71.0):
            new = next_state(state, value, HUMIDITY)
            if new is not None:
                changes.append(new)
                state = new
        self.assertEqual(changes, [ALARM, OK])


class IsReadingTest(unittest.TestCase):
    def test_numbers_are_readings(self):
        self.assertTrue(is_reading(25))
        self.assertTrue(is_reading(25.4))

    def test_anything_else_is_not(self):
        for value in (None, "25.4", True, [25.4], {"v": 25.4}):
            with self.subTest(value=value):
                self.assertFalse(is_reading(value))


if __name__ == "__main__":
    unittest.main()
