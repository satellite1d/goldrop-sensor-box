#!/usr/bin/env python3
"""One-time per-device management enrollment over trusted USB."""
import argparse
from pathlib import Path
import re
import sys
import time
from urllib.parse import urlsplit
from management_protocol import canonical_mac, key_id
from management_registry import Registry
from provision_wifi import normalize_server_host


class UsbSession:
    def __init__(self, port):
        import serial
        self.port = serial.Serial(port=None, baudrate=115200, timeout=.2, write_timeout=3)
        self.port.dtr = self.port.rts = False
        self.port.port = port
        self.port.open()

    def command(self, line, expected):
        data = (line + "\n").encode("utf-8")
        for offset in range(0, len(data), 16):
            self.port.write(data[offset:offset + 16])
            time.sleep(.06)
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            text = re.sub(r"\x1b\[[0-9;]*[A-Za-z]", "", self.port.read_until(b"\n", 4096).decode(errors="replace"))
            match = re.search(r"usb_config: (CFG .*)", text)
            if not match:
                continue
            reply = match[1].strip()
            if reply.startswith("CFG ERROR"):
                raise RuntimeError(reply)
            if reply.startswith(expected):
                return reply
        raise RuntimeError("Brak odpowiedzi USB; zamknij device monitor i sprawdz firmware/port")

    def close(self):
        self.port.close()


def management_endpoint(base, mac):
    parsed = urlsplit(base)
    if (parsed.scheme not in ("http", "https") or not base.startswith(("http://", "https://")) or
            not parsed.hostname or parsed.username or parsed.password or parsed.query or parsed.fragment or
            parsed.path not in ("", "/") or not base.isascii() or any(ord(c) < 33 or c in "\\@" for c in base)):
        raise ValueError("Podaj adres bazowy HTTP(S), np. http://r.limanowa.org:8000")
    normalize_server_host(parsed.hostname)
    if parsed.netloc.endswith(":") or (parsed.port is not None and not 1 <= parsed.port <= 65535):
        raise ValueError("Nieprawidlowy port")
    return base.rstrip("/") + "/management/v1/" + canonical_mac(mac)


def main():
    parser = argparse.ArgumentParser(description="Klucz zarzadzania Goldrop: jednorazowa rejestracja USB")
    parser.add_argument("--port", required=True)
    parser.add_argument("--db", help="Prywatny rejestr, ktory nalezy bezpiecznie przeniesc na serwer")
    parser.add_argument("--url", help="Adres bazowy uslugi, np. http://r.limanowa.org:8000")
    parser.add_argument("--rotate", action="store_true", help="Jawnie zastap klucz i rejestr urzadzenia")
    parser.add_argument("--action", choices=("enroll", "status", "enable", "disable", "forget", "cancel", "recover"), default="enroll")
    args = parser.parse_args()
    if args.action == "enroll" and (not args.db or not args.url):
        parser.error("Rejestracja wymaga --db i --url")
    usb = UsbSession(args.port)
    try:
        status = usb.command("manage status", "CFG MANAGE ")
        fields = dict(item.split("=", 1) for item in status.split() if "=" in item)
        print(status)
        if args.action == "status":
            return 0
        if args.action != "enroll":
            command = {"enable": "manage enable 1", "disable": "manage enable 0", "forget": "manage forget",
                       "cancel": "config cancel", "recover": "config recover"}[args.action]
            print(usb.command(command, "CFG OK CONFIG"))
            print(usb.command("manage status", "CFG MANAGE "))
            return 0
        mac = canonical_mac(fields["mac"])
        endpoint = management_endpoint(args.url, mac)
        registry = Registry(args.db)
        existing = registry.status(mac)
        if existing and not args.rotate:
            expected = existing[0]["key_id"]
            if fields.get("enrolled") != "true" or fields.get("key_id") != expected:
                raise ValueError("Rejestr i plytka nie maja tego samego klucza. Po resecie/utracie klucza uzyj --rotate.")
        elif fields.get("enrolled") == "true" and not args.rotate:
            raise ValueError("Plytka jest juz zarejestrowana. Uzyj jej prywatnego rejestru lub jawnego --rotate.")
        key = registry.enroll(mac, args.rotate)
        usb.command("manage url " + endpoint, "CFG OK MANAGE FIELD")
        usb.command("manage key " + key.hex(), "CFG OK MANAGE FIELD")
        print(usb.command("manage save", "CFG OK CONFIG"))
        status = usb.command("manage status", "CFG MANAGE ")
        if "key_id=" + key_id(key) not in status:
            raise RuntimeError("Nie potwierdzono zapisu klucza")
        print(status)
        print("Rejestracja zakonczona. Uzyj export-enrollment/import-enrollment i SCP, aby dodac klucz na serwerze; instrukcja: docs/zdalna-konfiguracja.md.")
        return 0
    finally:
        usb.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except ImportError:
        print("Zainstaluj tools/requirements-management.txt w srodowisku Python.", file=sys.stderr)
        sys.exit(1)
    except (ValueError, RuntimeError, OSError, KeyError) as error:
        print(str(error), file=sys.stderr)
        sys.exit(1)
    except (KeyboardInterrupt, EOFError):
        print("Przerwano.", file=sys.stderr)
        sys.exit(1)
