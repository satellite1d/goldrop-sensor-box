"""Bounded JSON-lines telemetry and durable ACK1 receipts (prototype TCP)."""

import json
import math
import re
from device_identity import canonical_mac

MAX_LINE_BYTES = 1024
MAX_U64 = (1 << 64) - 1
MAX_I64 = (1 << 63) - 1
MIN_TIMESTAMP_MS = 1704067200000
MAX_TIMESTAMP_MS = 4102444800000
MEASUREMENT_FIELDS = (
    "device_id",
    "boot_id",
    "sequence",
    "uptime_ms",
    "timestamp_ms",
    "temperature_sht_c",
    "humidity_rh",
    "temperature_ds18_c",
    "sht45_valid",
    "ds18b20_valid",
    "sht45_error",
    "ds18b20_error",
)


def integer(value, low, high):
    if type(value) is not int or not low <= value <= high:
        raise ValueError("Nieprawidlowa liczba calkowita")
    return value


def identifier(value, nonzero=False):
    if (
        not isinstance(value, str)
        or not re.fullmatch(r"(?:0|[1-9][0-9]{0,19})", value)
        or int(value) > MAX_U64
        or (nonzero and value == "0")
    ):
        raise ValueError("Nieprawidlowy identyfikator pomiaru")
    return value


def text(value, capacity):
    if (
        not isinstance(value, str)
        or not 1 <= len(value) <= capacity
        or any(ord(c) < 32 or ord(c) > 126 for c in value)
    ):
        raise ValueError("Nieprawidlowe pole tekstowe")
    return value


def boolean(value):
    if type(value) is not bool:
        raise ValueError("Nieprawidlowy stan logiczny")
    return value


def reading(value, valid, low, high):
    if not valid:
        if value is not None:
            raise ValueError("Niewazny odczyt musi byc null")
        return None
    if type(value) not in (int, float) or not low <= value <= high or not math.isfinite(value):
        raise ValueError("Odczyt poza zakresem czujnika")
    return float(value)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Powtorzone pole JSON")
        result[key] = value
    return result


def parse_message(line):
    if not isinstance(line, bytes) or not line.endswith(b"\n") or len(line) > MAX_LINE_BYTES:
        raise ValueError("Niekompletna lub zbyt dluga ramka")
    try:

        def invalid_constant(_):
            raise ValueError("Niedozwolona stala JSON")

        raw = json.loads(line, object_pairs_hook=unique_object, parse_constant=invalid_constant)
        if (
            not isinstance(raw, dict)
            or type(raw.get("schema_version")) is not int
            or raw["schema_version"] != 1
        ):
            raise ValueError("Nieobslugiwany schemat")
        mac = canonical_mac(raw.get("device_id"))
        kind = raw.get("message_type", "measurement")
        if kind == "status":
            result = {"device_id": mac, "message_type": "status"}
            for field in (
                "firmware_version",
                "hardware_revision",
                "ota_state",
                "ota_error",
                "delivery_confirmation",
                "config_phase",
                "config_result",
                "config_error",
                "config_revision",
                "config_pending_revision",
            ):
                if field in raw:
                    result[field] = text(raw[field], 64)
            for field, high in (
                ("ota_progress", 100),
                ("buffered", 1000000),
                ("sampling_interval_ms", 3600000),
            ):
                if field in raw:
                    result[field] = integer(raw[field], 0, high)
            for field in ("persistent_history", "utc_synchronized"):
                if field in raw:
                    result[field] = boolean(raw[field])
            return result
        if kind != "measurement":
            raise ValueError("Nieznany rodzaj wiadomosci")
        result = {
            "device_id": mac,
            "message_type": "measurement",
            "boot_id": identifier(raw["boot_id"]),
            "sequence": identifier(raw["sequence"], True),
            "uptime_ms": integer(raw["uptime_ms"], 0, MAX_I64),
            "timestamp_ms": (
                None
                if raw["timestamp_ms"] is None
                else integer(raw["timestamp_ms"], MIN_TIMESTAMP_MS, MAX_TIMESTAMP_MS)
            ),
        }
        for sensor in ("sht45", "ds18b20"):
            result[sensor + "_valid"] = boolean(raw[sensor + "_valid"])
            result[sensor + "_error"] = text(raw[sensor + "_error"], 32)
            if result[sensor + "_valid"] != (result[sensor + "_error"] == "none"):
                raise ValueError("Niespojny stan czujnika")
        result["temperature_sht_c"] = reading(
            raw["temperature_sht_c"], result["sht45_valid"], -40, 125
        )
        result["humidity_rh"] = reading(raw["humidity_rh"], result["sht45_valid"], 0, 100)
        result["temperature_ds18_c"] = reading(
            raw["temperature_ds18_c"], result["ds18b20_valid"], -55, 125
        )
        return result
    except (KeyError, TypeError, UnicodeError, RecursionError, json.JSONDecodeError) as error:
        raise ValueError("Nieprawidlowa wiadomosc telemetrii") from error


def acknowledgement(message):
    return f"ACK1 {message['device_id']} {message['boot_id']} {message['sequence']}\n".encode(
        "ascii"
    )
