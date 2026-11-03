#!/usr/bin/env python3
"""Goldrop: TCP -> SQLite -> a dashboard over an SSH tunnel with device naming."""

import argparse
import csv
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import io
import json
import logging
from pathlib import Path
import signal
import socket
import socketserver
import sqlite3
import threading
import time
from urllib.parse import urlsplit, parse_qs
from measurement_store import MeasurementStore
from telemetry_protocol import MAX_LINE_BYTES, parse_message, acknowledgement, unique_object

LOG = logging.getLogger("measurements")
MAX_RANGE_MS = 7 * 24 * 3600 * 1000


class LimitedThreads:
    daemon_threads = True
    request_queue_size = 128

    def process_request(self, request, address):
        if not self.slots.acquire(blocking=False):
            self.shutdown_request(request)
            return
        try:
            super().process_request(request, address)
        except BaseException:
            self.slots.release()
            raise

    def process_request_thread(self, request, address):
        try:
            super().process_request_thread(request, address)
        finally:
            self.slots.release()


class TelemetryServer(LimitedThreads, socketserver.ThreadingTCPServer):
    allow_reuse_address = True

    def __init__(self, address, store):
        self.store = store
        self.slots = threading.BoundedSemaphore(128)
        self.frame_timeout = 45
        self.stopping = threading.Event()
        super().__init__(address, TelemetryHandler)


class TelemetryHandler(socketserver.BaseRequestHandler):
    def handle(self):
        buffer = bytearray()
        deadline = time.monotonic() + self.server.frame_timeout
        try:
            while not self.server.stopping.is_set():
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    return
                self.request.settimeout(min(remaining, 5))
                try:
                    data = self.request.recv(MAX_LINE_BYTES + 1 - len(buffer))
                except socket.timeout:
                    continue
                if not data:
                    return
                buffer.extend(data)
                while b"\n" in buffer:
                    end = buffer.index(10) + 1
                    line = bytes(buffer[:end])
                    del buffer[:end]
                    message = parse_message(line)
                    self.server.store.save(message)
                    if message["message_type"] == "measurement":
                        # save() returns only after COMMIT; duplicates are confirmed too.
                        self.request.sendall(acknowledgement(message))
                    deadline = time.monotonic() + self.server.frame_timeout
                if len(buffer) >= MAX_LINE_BYTES:
                    return
        except ValueError:
            LOG.warning("Odrzucono nieprawidlowa lub konfliktowa ramke; brak ACK")
        except sqlite3.Error:
            LOG.error("Nie potwierdzono zapisu do bazy; urzadzenie zachowa pomiar")
        except OSError:
            pass  # A lost receipt is recovered by deduplication on the next connection.


class DashboardServer(LimitedThreads, ThreadingHTTPServer):
    def __init__(self, address, store, management=None):
        self.store = store
        self.management = management
        self.slots = threading.BoundedSemaphore(16)
        self.web = Path(__file__).resolve().parent / "web"
        super().__init__(address, DashboardHandler)


class DashboardHandler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def setup(self):
        self.request.settimeout(5)
        super().setup()

    def log_message(self, *_):
        pass

    def send_headers(self, status, kind, length=None, download=False):
        self.send_response(status)
        self.send_header("Content-Type", kind)
        if length is not None:
            self.send_header("Content-Length", str(length))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header(
            "Content-Security-Policy",
            "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'none'",
        )
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Connection", "close")
        if download:
            self.send_header("Content-Disposition", 'attachment; filename="goldrop-pomiary.csv"')
        self.end_headers()
        self.close_connection = True

    def send(self, status, body, kind="application/json; charset=utf-8", download=False):
        self.send_headers(status, kind, len(body), download)
        self.wfile.write(body)

    def export_range_csv(self, mac, start, end):
        fields = (
            "device_id",
            "boot_id",
            "sequence",
            "timestamp_ms",
            "received_ms",
            "uptime_ms",
            "temperature_sht_c",
            "humidity_rh",
            "temperature_ds18_c",
            "sht45_error",
            "ds18b20_error",
        )
        with self.server.store.export_range(mac, start, end) as rows:
            self.send_headers(200, "text/csv; charset=utf-8", download=True)
            self.csv_started = True
            output = io.StringIO()
            writer = csv.writer(output, delimiter=";")
            writer.writerow(fields)
            self.wfile.write(b"\xef\xbb\xbf")
            for index, row in enumerate(rows, 1):
                writer.writerow(
                    [
                        (
                            "'" + value
                            if isinstance(value, str) and value.startswith(("=", "+", "-", "@"))
                            else value
                        )
                        for value in (row[field] for field in fields)
                    ]
                )
                if index % 512 == 0:
                    self.wfile.write(output.getvalue().encode("utf-8"))
                    output.seek(0)
                    output.truncate(0)
            self.wfile.write(output.getvalue().encode("utf-8"))

    def do_POST(self):
        try:
            host = self.headers.get("Host", "")
            if (
                urlsplit("http://" + host).hostname not in ("localhost", "127.0.0.1")
                or self.headers.get("X-Goldrop-Request") != "1"
                or self.headers.get("Origin", "http://" + host) != "http://" + host
            ):
                self.send(403, b'{"error":"Niedozwolone zrodlo zadania"}')
                return
            if self.path not in ("/api/device/name", "/api/management/command"):
                self.send(404, b'{"error":"Nie znaleziono"}')
                return
            lengths = self.headers.get_all("Content-Length")
            if (
                self.headers.get("Transfer-Encoding")
                or not lengths
                or len(lengths) != 1
                or not lengths[0].isdigit()
                or not 0 < int(lengths[0]) <= 8192
                or self.headers.get("Content-Type") != "application/json"
            ):
                raise ValueError("Nieprawidlowa ramka")
            body = self.rfile.read(int(lengths[0]))
            if len(body) != int(lengths[0]):
                raise ValueError("Niekompletne zadanie")
            value = json.loads(body, object_pairs_hook=unique_object)
            if self.path == "/api/management/command":
                if self.server.management is None:
                    self.send(503, b'{"error":"Zarzadzanie nie jest wlaczone na serwerze"}')
                    return
                result = self.server.management.command(value)
                self.send(200, json.dumps(result, separators=(",", ":")).encode())
                return
            if not isinstance(value, dict) or set(value) != {"device_id", "name"}:
                raise ValueError("Nieprawidlowe pola")
            self.server.store.name(value["device_id"], value["name"])
            self.send(200, b'{"ok":true}')
        except (ValueError, TypeError, UnicodeError, KeyError, RecursionError):
            self.send(
                400,
                b'{"error":"Nieprawidlowe dane lub stan plytki. Sprawdz rejestracje, wersje firmware i oczekujace operacje."}',
            )
        except sqlite3.Error:
            self.send(503, b'{"error":"Nie zapisano zmiany; baza niedostepna"}')
        except OSError:
            self.close_connection = True

    def do_GET(self):
        self.csv_started = False
        try:
            host = urlsplit("http://" + self.headers.get("Host", "")).hostname
            if host not in ("localhost", "127.0.0.1"):
                self.send(403, b'{"error":"Uzyj localhost przez tunel SSH"}')
                return
            url = urlsplit(self.path)
            static = {
                "/": ("index.html", "text/html; charset=utf-8"),
                "/app.js": ("app.js", "text/javascript; charset=utf-8"),
                "/management.js": ("management.js", "text/javascript; charset=utf-8"),
                "/styles.css": ("styles.css", "text/css; charset=utf-8"),
            }
            if url.path in static and not url.query:
                name, kind = static[url.path]
                self.send(200, (self.server.web / name).read_bytes(), kind)
                return
            if url.path == "/api/management" and not url.query:
                result = (
                    self.server.management.snapshot()
                    if self.server.management
                    else dict(enabled=False, devices=[], releases=[])
                )
                self.send(200, json.dumps(result, separators=(",", ":")).encode())
                return
            if url.path == "/api/devices" and not url.query:
                result = {
                    "devices": self.server.store.devices(),
                    "server_time_ms": int(time.time() * 1000),
                }
            elif url.path in (
                "/api/measurements",
                "/api/chart",
                "/api/export.csv",
                "/api/export-range.csv",
            ):
                query = parse_qs(url.query, strict_parsing=True, max_num_fields=8)
                if set(query) - {"device_id", "from_ms", "to_ms", "limit", "before_id"} or any(
                    len(v) != 1 for v in query.values()
                ):
                    raise ValueError("Nieprawidlowy filtr")
                mac = query["device_id"][0]
                start = int(query["from_ms"][0])
                end = int(query["to_ms"][0])
                limit = int(query.get("limit", ["200"])[0])
                before = int(query["before_id"][0]) if "before_id" in query else None
                if (
                    start < 0
                    or not start < end
                    or end - start > MAX_RANGE_MS
                    or not 1 <= limit <= 1000
                ):
                    raise ValueError("Zakres maksymalnie 7 dni, do 1000 wierszy na strone")
                if url.path == "/api/export-range.csv":
                    if "limit" in query or "before_id" in query:
                        raise ValueError("Eksport zakresu nie przyjmuje stronicowania")
                    self.export_range_csv(mac, start, end)
                    return
                if url.path == "/api/chart":
                    result = self.server.store.chart(mac, start, end)
                else:
                    result = self.server.store.measurements(mac, start, end, limit, before)
                    if url.path.endswith(".csv"):
                        fields = (
                            "device_id",
                            "boot_id",
                            "sequence",
                            "timestamp_ms",
                            "received_ms",
                            "uptime_ms",
                            "temperature_sht_c",
                            "humidity_rh",
                            "temperature_ds18_c",
                            "sht45_error",
                            "ds18b20_error",
                        )
                        output = io.StringIO()
                        writer = csv.writer(output, delimiter=";")
                        writer.writerow(fields)
                        for row in result["items"]:
                            writer.writerow(
                                [
                                    (
                                        "'" + v
                                        if isinstance(v, str) and v.startswith(("=", "+", "-", "@"))
                                        else v
                                    )
                                    for v in (row[field] for field in fields)
                                ]
                            )
                        self.send(
                            200,
                            b"\xef\xbb\xbf" + output.getvalue().encode("utf-8"),
                            "text/csv; charset=utf-8",
                            True,
                        )
                        return
            else:
                self.send(404, b'{"error":"Nie znaleziono"}')
                return
            self.send(
                200, json.dumps(result, separators=(",", ":"), allow_nan=False).encode("utf-8")
            )
        except (ValueError, KeyError, OverflowError):
            self.send(400, b'{"error":"Nieprawidlowe parametry"}')
        except sqlite3.Error:
            if self.csv_started:
                self.close_connection = True
                return
            self.send(503, b'{"error":"Baza chwilowo niedostepna"}')
        except OSError:
            self.close_connection = True


def main():
    parser = argparse.ArgumentParser(
        description="Zapis i przegladanie pomiarow Goldrop (SQLite, TCP z ACK)"
    )
    parser.add_argument("--db", required=True, help="Prywatny plik poza katalogiem OTA")
    sub = parser.add_subparsers(dest="action", required=True)
    serve = sub.add_parser("serve")
    serve.add_argument("--bind", default="0.0.0.0", help="Interfejs nasluchu pomiarow")
    serve.add_argument("--tcp-port", type=int, default=54321)
    serve.add_argument(
        "--web-port", type=int, default=8080, help="Panel tylko na 127.0.0.1; dostep przez SSH -L"
    )
    serve.add_argument(
        "--management-db", help="Prywatny rejestr kluczy; wlacza zarzadzanie w panelu"
    )
    serve.add_argument("--ota-dir", help="Katalog publicznych, podpisanych wydan OTA")
    name = sub.add_parser("name")
    name.add_argument("mac")
    name.add_argument("value")
    backup = sub.add_parser("backup")
    backup.add_argument("--output", required=True)
    args = parser.parse_args()
    store = MeasurementStore(args.db)
    if args.action == "name":
        store.name(args.mac, args.value)
        return 0
    if args.action == "backup":
        store.backup(args.output)
        print("Zapisano spojna kopie bazy.")
        return 0
    if not 1 <= args.tcp_port <= 65535 or not 1 <= args.web_port <= 65535:
        parser.error("Porty musza byc z zakresu 1–65535")
    management = None
    if bool(args.management_db) != bool(args.ota_dir):
        parser.error("Podaj razem --management-db i --ota-dir")
    if args.management_db:
        try:
            from fleet_management import FleetManagement
        except ImportError as error:
            raise ValueError(
                "Zainstaluj requirements-management.txt i uruchom odbiornik Pythonem z venv"
            ) from error
        management = FleetManagement(args.management_db, args.ota_dir, store)
    stop = threading.Event()
    for sig in (signal.SIGINT, signal.SIGTERM):
        signal.signal(sig, lambda *_: stop.set())
    with TelemetryServer((args.bind, args.tcp_port), store) as tcp, DashboardServer(
        ("127.0.0.1", args.web_port), store, management
    ) as web:
        workers = [
            threading.Thread(target=server.serve_forever, daemon=True) for server in (tcp, web)
        ]
        for worker in workers:
            worker.start()
        LOG.info(
            "Pomiary TCP: %s:%d; panel: http://127.0.0.1:%d",
            args.bind,
            args.tcp_port,
            args.web_port,
        )
        stop.wait()
        tcp.stopping.set()
        for server in (tcp, web):
            server.shutdown()
        for worker in workers:
            worker.join()
    return 0


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
    try:
        raise SystemExit(main())
    except (ValueError, OSError, sqlite3.Error) as error:
        LOG.error("Nie mozna uruchomic uslugi: %s", error)
        raise SystemExit(1)
