"""Real sockets and SQLite, including C++ firmware transport -> Python -> COMMIT -> ACK."""

from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import csv
import http.client
import io
import json
from pathlib import Path
import socket
import sqlite3
import subprocess
import sys
import tarfile
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from measurement_store import MeasurementStore
import serve_measurements
from serve_measurements import TelemetryServer, DashboardServer
from telemetry_protocol import parse_message, acknowledgement

CPP = sys.argv.pop() if len(sys.argv) > 1 else None
MAC = "02:00:0a:10:ef:ff"
NOW = 1793000000000


def sample(**changes):
    value = dict(
        schema_version=1,
        device_id=MAC,
        boot_id="99",
        sequence="42",
        uptime_ms=1000,
        timestamp_ms=NOW,
        temperature_sht_c=23.5,
        humidity_rh=40,
        temperature_ds18_c=None,
        sht45_valid=True,
        ds18b20_valid=False,
        sht45_error="none",
        ds18b20_error="not_initialized",
    )
    value.update(changes)
    return value


def wire(value):
    return (json.dumps(value, separators=(",", ":")) + "\n").encode()


@contextmanager
def running(store):
    tcp = TelemetryServer(("127.0.0.1", 0), store)
    web = DashboardServer(("127.0.0.1", 0), store)
    tcp.daemon_threads = web.daemon_threads = False
    workers = [
        threading.Thread(target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True)
        for server in (tcp, web)
    ]
    for worker in workers:
        worker.start()
    try:
        yield tcp, web
    finally:
        tcp.stopping.set()
        for server in (tcp, web):
            server.shutdown()
            server.server_close()
        for worker in workers:
            worker.join()


def push(tcp, body):
    with socket.create_connection(("127.0.0.1", tcp.server_address[1]), timeout=5) as client:
        client.sendall(body)
        result = b""
        while b"\n" not in result:
            chunk = client.recv(128)
            if not chunk:
                break
            result += chunk
        return result


def http_request(web, path, method="GET", headers=None, body=None):
    client = http.client.HTTPConnection("127.0.0.1", web.server_port, timeout=3)
    try:
        client.request(method, path, body=body, headers=headers or {})
        response = client.getresponse()
        return response.status, dict(response.getheaders()), response.read()
    finally:
        client.close()


class BackendTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.store = MeasurementStore(self.root / "private" / "measurements.sqlite")

    def tearDown(self):
        self.tmp.cleanup()

    def test_validation_rejects_bad_data(self):
        for changes in (
            {"sequence": "18446744073709551616"},
            {"boot_id": 1},
            {"humidity_rh": 101},
            {"temperature_sht_c": 10**400},
            {"temperature_sht_c": float("nan")},
            {"ds18b20_valid": True},
            {"sht45_valid": 1},
            {"device_id": "03:00:00:00:00:01"},
            {"timestamp_ms": True},
            {"schema_version": True},
        ):
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                parse_message(wire(sample(**changes)))
        with self.assertRaises(ValueError):
            parse_message(wire(sample())[:-1])
        with self.assertRaises(ValueError):
            parse_message(b'{"schema_version":1,"schema_version":1}\n')
        with self.assertRaises(ValueError):
            parse_message(b"x" * 1024 + b"\n")

    def test_uint64_missing_sensor_and_durability(self):
        message = parse_message(
            wire(sample(boot_id=str((1 << 64) - 1), sequence=str((1 << 64) - 1), timestamp_ms=None))
        )
        self.assertTrue(self.store.save(message, NOW))
        self.assertFalse(self.store.save(message, NOW + 1))
        reopened = MeasurementStore(self.store.path)
        rows = reopened.measurements(MAC, NOW - 1, NOW + 10)["items"]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["sequence"], str((1 << 64) - 1))
        self.assertIsNone(rows[0]["timestamp_ms"])
        self.assertIsNone(rows[0]["temperature_ds18_c"])
        self.assertIs(rows[0]["sht45_valid"], True)
        self.assertEqual(rows[0]["received_ms"], NOW)
        with reopened.connection() as db:
            self.assertEqual(db.execute("PRAGMA synchronous").fetchone()[0], 2)
            self.assertEqual(db.execute("PRAGMA journal_mode").fetchone()[0], "wal")
        self.assertEqual(self.store.path.stat().st_mode & 0o777, 0o600)

    def test_conflicting_duplicate_rolls_back(self):
        self.store.save(parse_message(wire(sample())), NOW)
        with self.assertRaises(ValueError):
            self.store.save(parse_message(wire(sample(temperature_sht_c=30))), NOW + 10)
        self.assertEqual(self.store.devices()[0]["last_seen_ms"], NOW)
        self.assertEqual(len(self.store.measurements(MAC, NOW - 1, NOW + 100)["items"]), 1)

    def test_history_pagination_chart_and_names(self):
        for i in range(4):
            self.store.save(
                parse_message(
                    wire(
                        sample(
                            sequence=str(i + 1),
                            timestamp_ms=NOW + i * 1000,
                            temperature_sht_c=20 + i,
                        )
                    )
                ),
                NOW + i * 1000,
            )
        self.store.save(parse_message(wire(sample(sequence="5", timestamp_ms=None))), NOW + 5000)
        first = self.store.measurements(MAC, NOW, NOW + 6000, 2)
        second = self.store.measurements(MAC, NOW, NOW + 6000, 2, first["next_before"])
        self.assertEqual(
            [r["sequence"] for r in first["items"] + second["items"]], ["5", "4", "3", "2"]
        )
        chart = self.store.chart(MAC, NOW, NOW + 6000, 1)
        self.assertEqual(len(chart["items"]), 1)
        self.assertEqual(chart["items"][0]["samples"], 4)
        self.assertEqual(chart["items"][0]["temperature_sht_c"], 21.5)
        self.assertEqual(chart["items"][0]["min_sht"], 20)
        self.assertEqual(chart["items"][0]["max_sht"], 23)
        self.store.name(MAC, "Chłodnia 1")
        self.store.save(parse_message(wire(sample(sequence="6", timestamp_ms=None))), NOW + 6000)
        self.assertEqual(self.store.devices()[0]["name"], "Chłodnia 1")

    def test_wrong_database_and_unknown_version_preserved(self):
        bad = self.root / "private" / "other.sqlite"
        with sqlite3.connect(bad) as db:
            db.execute("CREATE TABLE other(key TEXT)")
            db.execute("PRAGMA user_version=1")
        bad.chmod(0o600)
        before = bad.read_bytes()
        with self.assertRaises(ValueError):
            MeasurementStore(bad)
        self.assertEqual(bad.read_bytes(), before)
        with self.store.connection() as db:
            db.execute("PRAGMA user_version=9")
        before = self.store.path.read_bytes()
        with self.assertRaises(ValueError):
            MeasurementStore(self.store.path)
        self.assertEqual(self.store.path.read_bytes(), before)

    def test_backup_live_database(self):
        self.store.save(parse_message(wire(sample())), NOW)
        target = self.root / "private" / "copy.sqlite"
        self.store.backup(target)
        copy = MeasurementStore(target)
        self.assertEqual(len(copy.measurements(MAC, NOW - 1, NOW + 1)["items"]), 1)
        with self.assertRaises(FileExistsError):
            self.store.backup(target)

    def test_ack_after_commit_and_duplicate(self):
        with running(self.store) as (tcp, _):
            expected = acknowledgement(parse_message(wire(sample())))
            self.assertEqual(push(tcp, wire(sample())), expected)
            self.assertEqual(push(tcp, wire(sample())), expected)
            self.assertEqual(
                len(MeasurementStore(self.store.path).measurements(MAC, NOW - 1, NOW + 1)["items"]),
                1,
            )

    def test_lost_ack_retried_without_duplicate(self):
        message = parse_message(wire(sample()))
        with running(self.store) as (tcp, _), patch.object(
            serve_measurements,
            "acknowledgement",
            side_effect=[OSError("lost receipt"), acknowledgement(message)],
        ):
            self.assertEqual(push(tcp, wire(sample())), b"")
            self.assertEqual(push(tcp, wire(sample())), acknowledgement(message))
        self.assertEqual(len(self.store.measurements(MAC, NOW - 1, NOW + 1)["items"]), 1)

    def test_failed_commit_never_acknowledged(self):
        original = self.store.connection

        @contextmanager
        def failed():
            with original() as db:

                class Proxy:
                    def execute(self, sql, *args):
                        if sql == "COMMIT":
                            raise sqlite3.OperationalError("injected disk failure")
                        return db.execute(sql, *args)

                yield Proxy()

        with running(self.store) as (tcp, _), patch.object(self.store, "connection", failed):
            self.assertEqual(push(tcp, wire(sample())), b"")
        self.assertEqual(self.store.devices(), [])
        with running(self.store) as (tcp, _):
            self.assertTrue(push(tcp, wire(sample())).startswith(b"ACK1 "))

    def test_partial_malformed_and_slow_frame(self):
        with running(self.store) as (tcp, _):
            self.assertEqual(push(tcp, b"bad\n"), b"")
            with socket.create_connection(
                ("127.0.0.1", tcp.server_address[1]), timeout=2
            ) as client:
                body = wire(sample())
                client.sendall(body[:30])
                client.settimeout(0.05)
                with self.assertRaises(socket.timeout):
                    client.recv(100)
                client.sendall(body[30:])
                client.settimeout(2)
                self.assertTrue(client.recv(100).startswith(b"ACK1 "))
            tcp.frame_timeout = 0.1
            with socket.create_connection(
                ("127.0.0.1", tcp.server_address[1]), timeout=1
            ) as client:
                client.sendall(b"{")
                self.assertEqual(client.recv(100), b"")

    def test_status_stored_without_creating_measurement(self):
        status = parse_message(
            wire(
                dict(
                    schema_version=1,
                    message_type="status",
                    device_id=MAC,
                    firmware_version="0.5.0",
                    buffered=10,
                    delivery_confirmation="backend_ack",
                    secret="discarded",
                )
            )
        )
        self.store.save(status, NOW)
        self.assertEqual(self.store.devices()[0]["status"]["firmware_version"], "0.5.0")
        self.assertNotIn("secret", self.store.devices()[0]["status"])
        self.assertEqual(self.store.measurements(MAC, NOW - 1, NOW + 1)["items"], [])

    def test_readonly_dashboard_csv_and_host(self):
        self.store.save(parse_message(wire(sample(ds18b20_error="=malicious"))), NOW)
        with running(self.store) as (_, web):
            for path in ("/", "/app.js", "/styles.css", "/api/devices"):
                code, headers, body = http_request(web, path)
                self.assertEqual(code, 200)
                self.assertTrue(body)
                self.assertIn("frame-ancestors 'none'", headers["Content-Security-Policy"])
            query = f"device_id={MAC}&from_ms={NOW-1}&to_ms={NOW+100}&limit=1"
            self.assertEqual(http_request(web, "/api/measurements?" + query)[0], 200)
            body = http_request(web, "/api/export.csv?" + query)[2].decode("utf-8-sig")
            rows = list(csv.reader(io.StringIO(body), delimiter=";"))
            self.assertEqual(rows[1][-1], "'=malicious")
            full = http_request(
                web, "/api/export-range.csv?" + f"device_id={MAC}&from_ms={NOW-1}&to_ms={NOW+100}"
            )
            self.assertEqual(
                list(csv.reader(io.StringIO(full[2].decode("utf-8-sig")), delimiter=";"))[1][-1],
                "'=malicious",
            )
            self.assertEqual(
                http_request(web, "/api/devices", headers={"Host": "evil.example"})[0], 403
            )
            self.assertEqual(http_request(web, "/api/devices", method="POST")[0], 403)
            self.assertEqual(http_request(web, "/../private/measurements.sqlite")[0], 404)
            self.assertEqual(http_request(web, "/api/measurements?" + query + "&limit=1")[0], 400)
            self.assertEqual(
                http_request(web, f"/api/chart?device_id={MAC}&from_ms=1&to_ms=99999999999")[0], 400
            )

    def test_rename_device_from_dashboard(self):
        self.store.save(parse_message(wire(sample())), NOW)
        other = "02:00:00:00:00:02"
        self.store.save(parse_message(wire(sample(device_id=other))), NOW)
        with running(self.store) as (_, web):
            headers = {
                "Content-Type": "application/json",
                "X-Goldrop-Request": "1",
                "Origin": f"http://127.0.0.1:{web.server_port}",
            }

            def rename(name, mac=MAC, custom=None):
                return http_request(
                    web,
                    "/api/device/name",
                    "POST",
                    headers if custom is None else custom,
                    json.dumps(dict(device_id=mac, name=name)).encode(),
                )

            self.assertEqual(rename("Chłodnia ąę")[0], 200)
            self.assertEqual(
                {d["device_id"]: d["name"] for d in self.store.devices()},
                {MAC: "Chłodnia ąę", other: ""},
            )
            for invalid in ("x" * 81, "bad\nname", "bad\x7fname", 123, None):
                self.assertEqual(rename(invalid)[0], 400)
            self.assertEqual(rename("test", "02:00:00:00:00:03")[0], 400)
            self.assertEqual(rename("bad", custom={"Content-Type": "application/json"})[0], 403)
            self.assertEqual(
                rename("bad", custom={**headers, "Origin": "https://evil.example"})[0], 403
            )
            self.assertEqual(rename("bad", custom={**headers, "Host": "evil.example"})[0], 403)
            self.assertEqual(
                {d["device_id"]: d["name"] for d in MeasurementStore(self.store.path).devices()}[
                    MAC
                ],
                "Chłodnia ąę",
            )
            self.assertEqual(rename("")[0], 200)
            self.assertEqual({d["device_id"]: d["name"] for d in self.store.devices()}[MAC], "")

    def test_full_csv_range_and_snapshot(self):
        for i in range(1205):
            self.store.save(
                parse_message(wire(sample(sequence=str(i + 1), timestamp_ms=NOW + i))), NOW + i
            )
        self.store.save(parse_message(wire(sample(sequence="1206", timestamp_ms=NOW - 1))), NOW)
        self.store.save(
            parse_message(wire(sample(device_id="02:00:00:00:00:02", sequence="1"))), NOW
        )
        with running(self.store) as (_, web):
            query = f"device_id={MAC}&from_ms={NOW}&to_ms={NOW+2000}"
            code, headers, body = http_request(web, "/api/export-range.csv?" + query)
            self.assertEqual(code, 200)
            self.assertIn("attachment", headers["Content-Disposition"])
            rows = list(csv.DictReader(io.StringIO(body.decode("utf-8-sig")), delimiter=";"))
            self.assertEqual(len(rows), 1205)
            self.assertEqual(rows[0]["sequence"], "1205")
            self.assertEqual(rows[-1]["sequence"], "1")
            self.assertEqual(len({r["sequence"] for r in rows}), 1205)
            self.assertEqual(
                http_request(web, "/api/export-range.csv?" + query + "&before_id=100")[0], 400
            )
            self.assertEqual(
                http_request(web, "/api/export-range.csv?" + query + "&limit=1")[0], 400
            )
            empty = http_request(
                web, f"/api/export-range.csv?device_id={MAC}&from_ms={NOW+3000}&to_ms={NOW+4000}"
            )
            self.assertEqual(len(empty[2].decode("utf-8-sig").splitlines()), 1)
        with self.store.export_range(MAC, NOW, NOW + 2000) as cursor:
            first = next(cursor)
            self.store.save(
                parse_message(wire(sample(sequence="1207", timestamp_ms=NOW + 1500))), NOW
            )
            self.assertEqual(1 + sum(1 for _ in cursor), 1205)
            self.assertEqual(first["sequence"], "1205")

    def test_cpp_firmware_to_python_database_roundtrip(self):
        if not CPP:
            self.skipTest("Pass C++ TCP adapter executable")
        with running(self.store) as (tcp, _):
            result = subprocess.run(
                [CPP, "--receiver", str(tcp.server_address[1])], capture_output=True, timeout=10
            )
            self.assertEqual(result.returncode, 0, result.stderr.decode())
        rows = self.store.measurements(MAC, 0, (1 << 63) - 1)["items"]
        self.assertEqual(len(rows), 1)
        self.assertEqual(rows[0]["temperature_sht_c"], 23.5)

    def test_packaged_cli_without_optional_dependencies(self):
        from package_management_server import package

        folder = self.root / "server"
        folder.mkdir()
        with tarfile.open(package(self.root / "server.tar.gz")) as archive:
            for member in archive.getmembers():
                # The archive is produced locally from the fixed source whitelist.
                self.assertFalse(Path(member.name).is_absolute())
                self.assertNotIn("..", Path(member.name).parts)
                archive.extract(member, folder)
        with socket.socket() as reserved_tcp, socket.socket() as reserved_web:
            reserved_tcp.bind(("127.0.0.1", 0))
            reserved_web.bind(("127.0.0.1", 0))
            tcp_port = reserved_tcp.getsockname()[1]
            web_port = reserved_web.getsockname()[1]
        database = self.root / "standalone" / "measurements.sqlite"
        command = [
            sys.executable,
            "-S",
            str(folder / "serve_measurements.py"),
            "--db",
            str(database),
        ]
        process = subprocess.Popen(
            command
            + [
                "serve",
                "--bind",
                "127.0.0.1",
                "--tcp-port",
                str(tcp_port),
                "--web-port",
                str(web_port),
            ],
            cwd=folder,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        try:
            deadline = time.monotonic() + 5
            while True:
                try:
                    with socket.create_connection(("127.0.0.1", tcp_port), timeout=0.2) as client:
                        client.settimeout(3)
                        client.sendall(wire(sample()))
                        self.assertEqual(
                            client.recv(100), acknowledgement(parse_message(wire(sample())))
                        )
                    break
                except ConnectionRefusedError:
                    if process.poll() is not None or time.monotonic() >= deadline:
                        self.fail("Packaged server did not start")
                    time.sleep(0.02)
            client = http.client.HTTPConnection("127.0.0.1", web_port, timeout=3)
            try:
                client.request("GET", "/")
                response = client.getresponse()
                self.assertEqual(response.status, 200)
                self.assertIn(b"/app.js", response.read())
            finally:
                client.close()
            backup = database.parent / "copy.sqlite"
            result = subprocess.run(
                command + ["backup", "--output", str(backup)], capture_output=True, timeout=5
            )
            self.assertEqual(result.returncode, 0, result.stderr.decode())
            self.assertEqual(len(MeasurementStore(backup).devices()), 1)
        finally:
            process.terminate()
            try:
                _, stderr = process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                raise
        self.assertEqual(process.returncode, 0, stderr.decode())

    def test_hundred_devices(self):
        with running(self.store) as (tcp, _):

            def send(i):
                mac = f"02:00:00:00:00:{i:02x}"
                body = wire(sample(device_id=mac, sequence="1"))
                self.assertEqual(push(tcp, body), f"ACK1 {mac} 99 1\n".encode())

            with ThreadPoolExecutor(max_workers=16) as pool:
                list(pool.map(send, range(100)))
        self.assertEqual(len(self.store.devices()), 100)


if __name__ == "__main__":
    unittest.main()
