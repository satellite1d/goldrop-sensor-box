"""Synthetic fleet: encrypted commands, group transactions, selected OTA and dashboard HTTP."""

from contextlib import contextmanager
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch

CPP = sys.argv.pop() if len(sys.argv) > 1 else None
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from test_measurement_backend import (
    sample,
    wire,
    parse_message,
    MeasurementStore,
    running,
    http_request,
    MAC,
    NOW,
)
from test_management import report
from test_package_ota import image
from management_protocol import seal, open_envelope, bind_challenge, make_command
from fleet_management import FleetManagement
from management_registry import Registry
from manage_devices import ManagementHTTPServer
from ota_signing import initialize_keys
from package_ota import package

OTHER = "02:00:00:00:00:02"


class FleetTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.ota = self.root / "ota"
        self.ota.mkdir()
        self.store = MeasurementStore(self.root / "private" / "measurements.sqlite")
        self.registry = Registry(self.root / "private" / "devices.sqlite")
        self.keys = {}
        for mac in (MAC, OTHER):
            self.keys[mac] = self.registry.enroll(mac)
            self.exchange(mac)
            self.store.save(parse_message(wire(sample(device_id=mac))), NOW)
            self.store.save(
                parse_message(
                    wire(
                        dict(
                            schema_version=1,
                            message_type="status",
                            device_id=mac,
                            firmware_version="0.6.0",
                            hardware_revision="goldrop-c3-v1.0",
                            sampling_interval_ms=10000,
                        )
                    )
                ),
                NOW,
            )
        self.fleet = FleetManagement(self.registry.path, self.ota, self.store)

    def tearDown(self):
        self.tmp.cleanup()

    def exchange(self, mac, version="0.6.0", **kwargs):
        plain = bytearray(report(**kwargs))
        plain[96:128] = version.encode().ljust(32, b"\0")
        key = self.keys[mac]
        return open_envelope(
            key, mac, b"S", self.registry.exchange(mac, seal(key, mac, b"C", bytes(plain))), 512
        )

    def release(self, version="0.6.1", targets=None):
        private = self.root / "keys" / "private.pem"
        public = self.root / "public.pem"
        if not private.exists():
            initialize_keys(private, public)
        data = image()
        data[48:80] = version.encode().ljust(32, b"\0")
        data[332:364] = version.encode().ljust(32, b"\0")
        binary = self.root / "firmware.bin"
        binary.write_bytes(data + public.read_bytes())
        package(
            binary, self.ota, "http://r.limanowa.org:8000", targets=targets, signing_key=private
        )
        name = f"manifest-{version}.json"
        shutil.copyfile(self.ota / "manifest.json", self.ota / name)
        return name

    def test_current_release_replaces_previous_ota_policy(self):
        newer = self.release()
        self.exchange(MAC, version="0.5.1", seq=2)
        self.fleet.command(
            dict(action="ota_check", devices=[MAC], release=newer, confirm_buffer_loss=True)
        )
        current = self.release("0.6.0")
        # A current version must pin that version too, replacing the previously selected release.
        outcome = self.fleet.command(
            dict(action="ota_check", devices=[MAC], release=current, confirm_buffer_loss=True)
        )
        self.assertEqual(outcome["results"][0]["mode"], "already_current")
        self.assertEqual(self.registry.ota_target(MAC), current)
        self.registry.queue(OTHER, "stage", ssid="future", password="password-test")
        with self.assertRaises(ValueError):
            self.fleet.command(
                dict(
                    action="ota_check",
                    devices=[MAC, OTHER],
                    release=newer,
                    confirm_buffer_loss=True,
                )
            )
        self.assertEqual(self.registry.ota_target(MAC), current)

    def test_catalog_malformed_types_recursion_and_duplicates(self):
        name = self.release()
        path = self.ota / name
        envelope = json.loads(path.read_bytes())
        original = json.loads(envelope["signed_manifest"])
        for field, value in (
            ("binary_url", 123),
            ("binary_url", []),
            ("sha256", []),
            ("schema_version", True),
            ("mandatory", "yes"),
            ("firmware_version", "00.6.1"),
        ):
            changed = {**envelope, "signed_manifest": json.dumps({**original, field: value})}
            path.write_text(json.dumps(changed))
            with self.assertRaises(ValueError):
                self.fleet.catalog.read(name)
            self.assertEqual(self.fleet.snapshot()["releases"], [])
        path.write_text("[" * 1500 + "0" + "]" * 1500)
        self.assertEqual(self.fleet.snapshot()["releases"], [])
        path.write_text('{"signature_schema":1,"signature_schema":1}')
        self.assertEqual(self.fleet.snapshot()["releases"], [])

    def test_catalog_limit_uses_numeric_version_order(self):
        for n in range(101):
            (self.ota / f"manifest-0.7.{n}.json").touch()
        with patch.object(
            self.fleet.catalog, "read", side_effect=lambda name: ({"version": name[9:-5]}, b"")
        ):
            releases = self.fleet.catalog.available()
        self.assertEqual(len(releases), 100)
        self.assertEqual(releases[0]["version"], "0.7.100")
        self.assertEqual(releases[-1]["version"], "0.7.1")

    def test_measurements_database_is_never_modified_as_registry(self):
        with self.assertRaises(ValueError):
            Registry(self.store.path)
        with self.store.connection() as db:
            self.assertFalse(
                db.execute("SELECT 1 FROM sqlite_master WHERE name='ota_targets'").fetchone()
            )
        self.assertEqual(len(self.store.devices()), 2)

    def test_sampling_does_not_scan_firmware_or_query_each_target(self):
        with patch.object(
            self.fleet.catalog, "available", side_effect=AssertionError("unnecessary hashing")
        ), patch.object(self.fleet.registry, "ota_target", side_effect=AssertionError("N+1 query")):
            self.fleet.command(dict(action="sampling", devices=[MAC], interval_seconds=60))

    def test_sampling_group_and_encrypted_interop(self):
        result = self.fleet.command(
            dict(action="sampling", devices=[MAC, OTHER], interval_seconds=60)
        )
        self.assertEqual(len(result["results"]), 2)
        for mac in (MAC, OTHER):
            command = self.exchange(mac)
            self.assertEqual(command[4], 7)
            self.assertEqual(struct.unpack_from("<I", command, 417)[0], 60000)
            if CPP:
                result = subprocess.run(
                    [CPP, "--decode"],
                    input=bind_challenge(command, bytes([0x42]) * 16),
                    capture_output=True,
                    timeout=3,
                )
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertTrue(result.stdout.decode().endswith("60000"))
        self.assertEqual(
            MeasurementStore(self.store.path).devices()[0]["status"]["sampling_interval_ms"], 10000
        )

    def test_group_is_atomic_when_one_device_busy_or_old(self):
        self.registry.queue(OTHER, "stage", ssid="future", password="test-password")
        with self.assertRaises(ValueError):
            self.fleet.command(dict(action="sampling", devices=[MAC, OTHER], interval_seconds=60))
        self.assertIsNone(self.registry.status(MAC)[0]["queued_action"])
        with self.registry.connect() as db:
            db.execute(
                "UPDATE devices SET command=NULL,command_action=NULL,command_revision=NULL WHERE mac=?",
                (OTHER,),
            )
        self.exchange(OTHER, version="0.5.1", seq=2)
        with self.assertRaises(ValueError):
            self.fleet.command(dict(action="sampling", devices=[MAC, OTHER], interval_seconds=60))
        self.assertIsNone(self.registry.status(MAC)[0]["queued_action"])

    def test_wifi_commands_do_not_leak_secrets(self):
        password = "private-test-password"
        result = self.fleet.command(
            dict(action="stage", devices=[MAC], ssid="future", password=password)
        )
        self.assertEqual(result["results"][0]["mode"], "command")
        self.assertNotIn(password, json.dumps(self.fleet.snapshot()))
        with self.registry.connect() as db:
            encrypted = db.execute("SELECT command FROM devices WHERE mac=?", (MAC,)).fetchone()[0]
        self.assertNotIn(password.encode(), encrypted)
        command = self.exchange(MAC)
        self.assertEqual(command[4], 2)
        self.assertEqual(command[97:161].split(b"\0")[0], password.encode())

    def test_ota_targets_old_client_and_restart(self):
        name = self.release()
        self.exchange(OTHER, version="0.5.1", seq=2)
        result = self.fleet.command(
            dict(action="ota_check", devices=[MAC, OTHER], release=name, confirm_buffer_loss=True)
        )
        self.assertEqual([r["mode"] for r in result["results"]], ["command", "automatic_poll"])
        self.assertEqual(self.exchange(MAC)[4], 8)
        reopened = Registry(self.registry.path)
        self.assertEqual(reopened.ota_target(OTHER), name)
        self.assertIsNone(reopened.status(OTHER)[0]["queued_action"])
        self.assertNotIn("key", self.fleet.snapshot()["devices"][0])

    def test_selected_manifest_http_and_bootstrap_unchanged(self):
        name = self.release()
        bootstrap = b'{"bootstrap":true}'
        (self.ota / "manifest.json").write_bytes(bootstrap)
        self.fleet.command(
            dict(action="ota_check", devices=[MAC], release=name, confirm_buffer_loss=True)
        )
        server = ManagementHTTPServer(("127.0.0.1", 0), self.registry, self.ota)
        worker = threading.Thread(
            target=server.serve_forever, kwargs={"poll_interval": 0.01}, daemon=True
        )
        worker.start()
        try:
            self.assertEqual(
                http_request(server, "/manifest.json", headers={"X-Goldrop-Device": MAC})[2],
                (self.ota / name).read_bytes(),
            )
            self.assertEqual(
                http_request(server, "/manifest.json", headers={"X-Goldrop-Device": OTHER})[2],
                bootstrap,
            )
            self.assertEqual(http_request(server, "/manifest.json")[2], bootstrap)
            self.assertEqual(http_request(server, "/../private/devices.sqlite")[0], 404)
        finally:
            server.shutdown()
            server.server_close()
            worker.join()

    def test_signed_targets_and_wrong_hardware_rejected(self):
        name = self.release(targets=[MAC])
        with self.assertRaises(ValueError):
            self.fleet.command(
                dict(
                    action="ota_check", devices=[MAC, OTHER], release=name, confirm_buffer_loss=True
                )
            )
        self.assertIsNone(self.registry.ota_target(MAC))
        with self.assertRaises(ValueError):
            self.fleet.command(
                dict(action="ota_check", devices=[MAC], release=name, confirm_buffer_loss=False)
            )
        self.store.save(
            parse_message(
                wire(
                    dict(
                        schema_version=1,
                        message_type="status",
                        device_id=MAC,
                        hardware_revision="other-board",
                    )
                )
            ),
            NOW,
        )
        with self.assertRaises(ValueError):
            self.fleet.command(
                dict(action="ota_check", devices=[MAC], release=name, confirm_buffer_loss=True)
            )

    def test_invalid_release_and_payloads(self):
        name = self.release()
        (self.ota / "manifest-0.6.2.json").write_text("[]")
        self.assertEqual(len(self.fleet.catalog.available()), 1)
        for devices in ([], [MAC, MAC], ["02:00:00:00:00:ff"]):
            with self.assertRaises(ValueError):
                self.fleet.command(dict(action="sampling", devices=devices, interval_seconds=60))
        for seconds in (True, 1, 3601):
            with self.assertRaises(ValueError):
                self.fleet.command(dict(action="sampling", devices=[MAC], interval_seconds=seconds))
        with self.assertRaises(ValueError):
            self.fleet.command(
                dict(
                    action="ota_check",
                    devices=[MAC],
                    release="../manifest.json",
                    confirm_buffer_loss=True,
                )
            )
        (self.ota / "goldrop-c3-v1.0-0.6.1.bin").write_bytes(b"bad")
        self.assertEqual(self.fleet.catalog.available(), [])

    def test_dashboard_http_security_and_status(self):
        with running(self.store) as (_, web):
            web.management = self.fleet
            self.assertTrue(json.loads(http_request(web, "/api/management")[2])["enabled"])
            headers = {
                "Content-Type": "application/json",
                "X-Goldrop-Request": "1",
                "Origin": f"http://127.0.0.1:{web.server_port}",
            }
            body = json.dumps(dict(action="sampling", devices=[MAC], interval_seconds=30)).encode()
            self.assertEqual(
                http_request(
                    web,
                    "/api/management/command",
                    "POST",
                    {**headers, "Origin": "https://evil.example"},
                    body,
                )[0],
                403,
            )
            self.assertIsNone(self.registry.status(MAC)[0]["queued_action"])
            self.assertEqual(
                http_request(web, "/api/management/command", "POST", headers, body)[0], 200
            )
            self.assertEqual(self.registry.status(MAC)[0]["queued_action"], "sampling")
            self.assertEqual(http_request(web, "/management.js")[0], 200)
            for invalid in (
                b"[" * 1500 + b"0" + b"]" * 1500,
                b'{"action":"cancel","action":"sampling","devices":[]}',
            ):
                self.assertEqual(
                    http_request(web, "/api/management/command", "POST", headers, invalid)[0], 400
                )
            self.assertEqual(http_request(web, "/api/management")[0], 200)


if __name__ == "__main__":
    unittest.main()
