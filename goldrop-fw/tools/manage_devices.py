#!/usr/bin/env python3
"""Serve OTA files and encrypted device management; administer locally over SSH."""

import argparse
import getpass
import io
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import re
import socket
import threading
import time
from urllib.parse import urlsplit
from cryptography.exceptions import InvalidTag
from management_protocol import canonical_mac
from management_registry import Registry
from firmware_catalog import FirmwareCatalog


class DeadlineReader(io.RawIOBase):
    """Apply one deadline across all header/body reads, including slow byte streams."""

    def __init__(self, connection, seconds):
        super().__init__()
        self.connection = connection
        self.raw = connection.makefile("rb", buffering=0)
        self.deadline = time.monotonic() + seconds

    def readable(self):
        return True

    def readinto(self, buffer):
        remaining = self.deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("HTTP request read deadline")
        self.connection.settimeout(min(5, remaining))
        return self.raw.readinto(buffer)

    def close(self):
        try:
            self.raw.close()
        finally:
            super().close()


class ManagementHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    request_queue_size = 128
    request_read_timeout = 15

    def __init__(self, address, registry, ota_directory):
        self.registry = registry
        self.ota_directory = Path(ota_directory).resolve(strict=True)
        if not self.ota_directory.is_dir():
            raise ValueError("Katalog publicznych plikow OTA nie istnieje")
        if self.ota_directory in registry.path.resolve().parents:
            raise ValueError("Rejestr kluczy musi byc poza katalogiem OTA")
        self.slots = threading.BoundedSemaphore(16)
        super().__init__(address, ManagementHandler)

    def process_request(self, request, client_address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, client_address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, client_address):
        try:
            request.settimeout(5)
            super().process_request_thread(request, client_address)
        finally:
            self.slots.release()


class ManagementHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        super().setup()
        self.rfile.close()
        self.rfile = io.BufferedReader(
            DeadlineReader(self.connection, self.server.request_read_timeout)
        )

    def log_message(self, *_):
        pass  # Never log URLs, request bodies, credentials or keys.

    def reply(self, status, body=b"", content_type="application/octet-stream"):
        self.connection.settimeout(5)
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        if self.command != "HEAD":
            self.wfile.write(body)

    def do_POST(self):
        try:
            match = re.fullmatch(r"/management/v1/((?:[0-9a-f]{2}:){5}[0-9a-f]{2})", self.path)
            lengths = self.headers.get_all("Content-Length")
            if not match or lengths != ["168"] or self.headers.get("Transfer-Encoding"):
                self.reply(400)
                return
            body = self.rfile.read(168)
            self.reply(200, self.server.registry.exchange(match[1], body))
        except (ValueError, InvalidTag):
            self.reply(403)
        except (OSError, TimeoutError):
            self.close_connection = True
        except Exception:
            # Fail closed without exception messages that might contain input.
            self.reply(503)

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        self.connection.settimeout(5)
        # Deliberately no directory listings, arbitrary files or symlink traversal.
        if not re.fullmatch(
            r"/(?:manifest(?:-[0-9.]+)?\.json|goldrop-[A-Za-z0-9_.-]+\.bin)", self.path
        ):
            self.reply(404)
            return
        requested = self.path[1:]
        try:
            device = self.headers.get("X-Goldrop-Device")
            if requested == "manifest.json" and device:
                selected = self.server.registry.ota_target(device)
                if selected:
                    requested = selected
            if requested.startswith("manifest-"):
                _, body = FirmwareCatalog(self.server.ota_directory).read(requested, device)
                self.reply(200, body, "application/json")
                return
        except (ValueError, KeyError, TypeError, OSError):
            self.reply(404)
            return
        path = self.server.ota_directory / requested
        if path.is_symlink() or not path.is_file():
            self.reply(404)
            return
        try:
            with path.open("rb") as source:
                self.send_response(200)
                self.send_header(
                    "Content-Type",
                    "application/json" if path.suffix == ".json" else "application/octet-stream",
                )
                self.send_header("Content-Length", str(os.fstat(source.fileno()).st_size))
                self.send_header("Cache-Control", "no-cache")
                self.send_header("Connection", "close")
                self.end_headers()
                self.close_connection = True
                if self.command != "HEAD":
                    while chunk := source.read(16384):
                        self.wfile.write(chunk)
        except OSError:
            self.close_connection = True


def main():
    parser = argparse.ArgumentParser(
        description="Zarzadzanie Goldrop przez SSH; hasla nie trafiaja do argumentow"
    )
    parser.add_argument(
        "--db",
        required=True,
        help="Prywatny rejestr SQLite, poza katalogiem OTA (katalog 700, plik 600)",
    )
    sub = parser.add_subparsers(dest="action", required=True)
    serve = sub.add_parser("serve")
    serve.add_argument("--ota-dir", required=True)
    serve.add_argument("--bind", default="0.0.0.0")
    serve.add_argument("--port", type=int, default=8000)
    status = sub.add_parser("status")
    status.add_argument("mac", nargs="?")
    export = sub.add_parser(
        "export-enrollment", help="Prywatny plik jednej plytki do przeniesienia przez SCP"
    )
    export.add_argument("mac")
    export.add_argument("--output", required=True)
    incoming = sub.add_parser(
        "import-enrollment", help="Dodaj klucz bez nadpisywania rejestru pozostalych plytek"
    )
    incoming.add_argument("--file", required=True)
    incoming.add_argument(
        "--replace",
        action="store_true",
        help="Po jawnej rotacji USB zastap poprzedni klucz tej plytki",
    )
    for name in ("apply", "stage", "activate", "arm", "cancel"):
        cmd = sub.add_parser(name)
        cmd.add_argument("mac")
        if name in ("apply", "stage"):
            cmd.add_argument("--wifi", action="store_true", help="Zapytaj o SSID i haslo")
            cmd.add_argument("--server", action="store_true", help="Zapytaj o DNS/IPv4 i port")
    args = parser.parse_args()
    registry = Registry(args.db)
    if args.action == "export-enrollment":
        registry.export_enrollment(args.mac, args.output)
        print("Zapisano prywatny plik rejestracji (600). Przenies go przez SCP poza katalog OTA.")
        return 0
    if args.action == "import-enrollment":
        mac = registry.import_enrollment(args.file, args.replace)
        print("Zaimportowano rejestracje: " + mac)
        return 0
    if args.action == "serve":
        with ManagementHTTPServer((args.bind, args.port), registry, args.ota_dir) as server:
            print(
                f"Goldrop OTA + management: port {server.server_port}; klucze poza katalogiem publicznym",
                flush=True,
            )
            try:
                server.serve_forever()
            except KeyboardInterrupt:
                pass
        return 0
    if args.action == "status":
        print(json.dumps(registry.status(args.mac), indent=2, ensure_ascii=False))
        return 0
    fields = {}
    if args.action in ("apply", "stage"):
        if not args.wifi and not args.server:
            parser.error("Podaj --wifi lub --server (mozna oba)")
        if args.wifi:
            fields["ssid"] = input("Nowe SSID (2,4 GHz): ")
            fields["password"] = getpass.getpass("Nowe haslo Wi-Fi: ")
        if args.server:
            fields["host"] = input("Nowy serwer DNS/IPv4: ").strip()
            port = input("Nowy port TCP: ").strip()
            if not re.fullmatch(r"[0-9]{1,5}", port) or not 1 <= int(port) <= 65535:
                raise ValueError("Podaj port TCP 1–65535")
            fields["port"] = int(port)
    revision = registry.queue(args.mac, args.action, **fields)
    print(
        f"Zlecono {args.action}, rewizja {revision}. Wynik sprawdz poleceniem status."
        if revision
        else "Brak aktywnego kandydata."
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (ValueError, OSError) as error:
        print(f"Blad: {error}")
        raise SystemExit(1)
    except (EOFError, KeyboardInterrupt):
        print("Przerwano.")
        raise SystemExit(1)
