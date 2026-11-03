"""Goldrop management v1. No passwords or keys are written to logs."""

import hashlib
import os
import re
import struct
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from provision_wifi import normalize_server_host
from device_identity import canonical_mac

MAX_REVISION = (1 << 63) - 1
PHASES = ("idle", "staged", "armed", "testing")
RESULTS = (
    "none",
    "staged",
    "armed",
    "testing",
    "committed",
    "rolled_back",
    "rejected",
    "cancelled",
    "local_change",
)
ERRORS = (
    "none",
    "bad_base",
    "busy",
    "no_candidate",
    "invalid",
    "timeout",
    "reboot",
    "apply",
    "storage",
    "local_override",
)
ACTIONS = {
    "none": 0,
    "apply": 1,
    "stage": 2,
    "activate": 3,
    "arm": 4,
    "cancel": 5,
    "confirm": 6,
    "sampling": 7,
    "ota_check": 8,
}


def key_id(key):
    return hashlib.sha256(key).hexdigest()[:16]


def direction_key(master, direction):
    if len(master) != 32 or not any(master) or direction not in (b"C", b"S"):
        raise ValueError("Invalid cryptographic context")
    return HKDF(
        algorithm=hashes.SHA256(),
        length=32,
        salt=b"goldrop-management-v1",
        info=b"request" if direction == b"C" else b"response",
    ).derive(master)


def context(mac, direction):
    return b"goldrop-config-v1\0" + bytes.fromhex(canonical_mac(mac).replace(":", "")) + direction


def seal(key, mac, direction, plaintext):
    nonce = os.urandom(12)
    return (
        b"GRC1"
        + nonce
        + AESGCM(direction_key(key, direction)).encrypt(nonce, plaintext, context(mac, direction))
    )


def open_envelope(key, mac, direction, envelope, size):
    if len(envelope) != size + 32 or envelope[:4] != b"GRC1":
        raise ValueError("Invalid envelope")
    return AESGCM(direction_key(key, direction)).decrypt(
        envelope[4:16], envelope[16:], context(mac, direction)
    )


def cstring(data):
    end = data.find(b"\0")
    if end < 0 or any(data[end:]):
        raise ValueError("Invalid string padding")
    return data[:end].decode("utf-8")


def parse_report(data):
    if (
        len(data) != 136
        or data[:4] != b"GQ01"
        or data[4] > 3
        or data[5] > 8
        or data[6] > 9
        or data[7] > 3
    ):
        raise ValueError("Invalid report")
    seq, high, active, pending, result_rev = struct.unpack_from("<5Q", data, 8)
    exchange_sequence = struct.unpack_from("<Q", data, 128)[0]
    if not 0 < exchange_sequence <= MAX_REVISION:
        raise ValueError("Invalid exchange counter")
    if (
        not 0 < seq <= MAX_REVISION
        or not 0 <= high <= MAX_REVISION
        or max(active, pending, result_rev) > high
    ):
        raise ValueError("Invalid report counters")
    if (data[4] == 0 and (pending or any(data[64:80]))) or (
        data[4] != 0 and (not pending or not any(data[64:80]))
    ):
        raise ValueError("Invalid transaction")
    version = cstring(data[96:128])
    if not re.fullmatch(r"[0-9]{1,9}\.[0-9]{1,9}\.[0-9]{1,9}", version):
        raise ValueError("Invalid firmware version")
    return dict(
        phase=PHASES[data[4]],
        result=RESULTS[data[5]],
        error=ERRORS[data[6]],
        wifi=bool(data[7] & 1),
        probe=bool(data[7] & 2),
        sequence=seq,
        highwater=high,
        active_revision=active,
        pending_revision=pending,
        result_revision=result_rev,
        operation=data[64:80].hex(),
        result_operation=data[48:64].hex(),
        firmware=version,
        exchange_sequence=exchange_sequence,
    )


def report_identity(data):
    # Flags and the fresh exchange challenge may change without an NVS write.
    return data[:7] + data[8:80] + data[96:128]


def wifi_fields(ssid, password):
    s = ssid.encode("utf-8")
    if not 1 <= len(s) <= 32 or any(b < 32 or b == 127 for b in s):
        raise ValueError("SSID musi miec 1–32 bajty bez znakow sterujacych")
    if not 8 <= len(password) <= 63 or any(not 32 <= ord(c) <= 126 for c in password):
        raise ValueError("Haslo musi miec 8–63 drukowalne znaki ASCII")
    return s.ljust(33, b"\0"), password.encode("ascii").ljust(64, b"\0")


def make_command(
    action,
    revision=0,
    base=0,
    target=0,
    operation=bytes(16),
    ssid=None,
    password=None,
    host=None,
    port=None,
    interval_ms=None,
):
    result = bytearray(512)
    result[:4] = b"GP01"
    result[4] = ACTIONS[action]
    if any(not 0 <= n <= MAX_REVISION for n in (revision, base, target)) or len(operation) != 16:
        raise ValueError("Invalid command counters")
    struct.pack_into("<3Q", result, 8, revision, base, target)
    result[32:48] = operation
    if ssid is not None:
        result[64:97], result[97:161] = wifi_fields(ssid, password)
        result[5] |= 1
    if host is not None:
        normalized = normalize_server_host(host).encode("ascii")
        if port is None or not 1 <= int(port) <= 65535:
            raise ValueError("Port musi byc w zakresie 1–65535")
        result[161:415] = normalized.ljust(254, b"\0")
        struct.pack_into("<H", result, 415, int(port))
        result[5] |= 2
    if action == "sampling":
        if type(interval_ms) is not int or not 10000 <= interval_ms <= 3600000:
            raise ValueError("Interwal musi wynosic 10–3600 sekund")
        struct.pack_into("<I", result, 417, interval_ms)
    elif interval_ms is not None:
        raise ValueError("Nieprawidlowy interwal")
    if action in ("sampling", "ota_check") and (result[5] or target):
        raise ValueError("Nieprawidlowe pola polecenia")
    if action in ("apply", "stage"):
        if not result[5] or target:
            raise ValueError("Brak zmian konfiguracji")
    elif result[5]:
        raise ValueError("Nieprawidlowe pola polecenia")
    if action != "none" and (
        not revision
        or not any(operation)
        or (action not in ("apply", "stage", "sampling", "ota_check") and not target)
    ):
        raise ValueError("Invalid command identity")
    return bytes(result)


def bind_challenge(command, challenge):
    if len(command) != 512 or len(challenge) != 16:
        raise ValueError("Invalid response size")
    return command[:48] + challenge + command[64:]
