"""Canonical Goldrop Wi-Fi station identity; no optional dependencies."""

import re


def canonical_mac(value):
    if not isinstance(value, str) or not re.fullmatch(
        r"(?:[0-9a-fA-F]{2}:){5}[0-9a-fA-F]{2}", value
    ):
        raise ValueError("Nieprawidlowy MAC")
    raw = bytes.fromhex(value.replace(":", ""))
    if not any(raw) or raw[0] & 1:
        raise ValueError("Nieprawidlowy MAC stacji Wi-Fi")
    return value.lower()
