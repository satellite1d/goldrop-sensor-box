#!/usr/bin/env python3
import concurrent.futures
import http.client
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import itertools
import io
from unittest.mock import patch
import tarfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from cryptography.exceptions import InvalidTag
from management_protocol import *
from management_registry import Registry
from manage_devices import ManagementHTTPServer
from provision_management import management_endpoint
import provision_management
from package_management_server import package

CPP = sys.argv.pop() if len(sys.argv) > 1 else None
MAC = "90:da:72:9b:cd:4c"
KEY = bytes(range(32))  # Public synthetic test key only.
EXCHANGES = itertools.count(1)


def report(seq=1, high=0, active=0, pending=0, phase=0, result=0, error=0, flags=3, operation=bytes(16), result_rev=0):
    out = bytearray(136)
    out[:4] = b"GQ01"
    out[4:8] = bytes((phase, result, error, flags))
    struct.pack_into("<5Q", out, 8, seq, high, active, pending, result_rev)
    out[48:64] = operation if result_rev else bytes(16)
    out[64:80] = operation if pending else bytes(16)
    out[80:96] = os.urandom(16)
    out[96:102] = b"0.4.0\0"
    struct.pack_into("<Q",out,128,next(EXCHANGES))
    return bytes(out)


class CryptoTests(unittest.TestCase):
    def test_aead_context_tampering_and_wrong_keys(self):
        body = seal(KEY, MAC, b"C", report())
        plain = open_envelope(KEY, MAC, b"C", body, 136)
        self.assertEqual(len(plain), 136)
        for key, mac, direction in ((bytes([5]) * 32, MAC, b"C"), (KEY, "90:da:72:9b:cd:4e", b"C"), (KEY, MAC, b"S")):
            with self.assertRaises(InvalidTag):
                open_envelope(key, mac, direction, body, 136)
        for i in range(4, len(body)):
            damaged = bytearray(body)
            damaged[i] ^= 1
            with self.assertRaises(InvalidTag):
                open_envelope(KEY, MAC, b"C", bytes(damaged), 136)
        for invalid in (body[:-1], body + b"x", b"BAD!" + body[4:]):
            with self.assertRaises(ValueError):
                open_envelope(KEY, MAC, b"C", invalid, 136)

    def test_nonce_and_hkdf_directions(self):
        self.assertNotEqual(direction_key(KEY, b"C"), direction_key(KEY, b"S"))
        nonces = {seal(KEY, MAC, b"C", report())[4:16] for _ in range(1000)}
        self.assertEqual(len(nonces), 1000)
        self.assertEqual(key_id(KEY), "630dcd2966c43366")

    def test_cpp_protocol_interoperability(self):
        if not CPP:
            self.skipTest("Pass C++ codec executable")
        command = make_command("apply", 123, 12, operation=bytes([1]) * 16,
                               ssid="test-siec", password="synthetic-only", host="R.Limanowa.ORG", port=54321)
        bound = bind_challenge(command, b"B" * 16)
        result = subprocess.run([CPP, "--decode"], input=bound, capture_output=True, check=True)
        self.assertEqual(result.stdout, b"1 123 3 14 r.limanowa.org 54321")
        for index in (0, 6, 48, 416, 417, 511):
            damaged = bytearray(bound)
            damaged[index] ^= 0x80 if index == 416 else 1
            if index == 416:  # A different nonzero port is valid, no AEAD here.
                continue
            self.assertEqual(subprocess.run([CPP, "--decode"], input=damaged, capture_output=True).returncode, 2)

    def test_validation(self):
        for ssid, password in (("", "12345678"), ("x" * 33, "12345678"), ("ok", "short"), ("a\n", "12345678")):
            with self.assertRaises(ValueError):
                wifi_fields(ssid, password)
        self.assertEqual(management_endpoint("http://r.limanowa.org:8000", MAC), "http://r.limanowa.org:8000/management/v1/" + MAC)
        for url in ("file:///tmp/key", "http://user:pass@example.com", "http://example.com/path", "http://example.com:"):
            with self.assertRaises(ValueError):
                management_endpoint(url, MAC)


class RegistryTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.registry = Registry(self.root / "private" / "devices.sqlite")
        self.key = self.registry.enroll(MAC)

    def tearDown(self):
        self.tmp.cleanup()

    def exchange(self, plain):
        envelope = self.registry.exchange(MAC, seal(self.key, MAC, b"C", plain))
        response = open_envelope(self.key, MAC, b"S", envelope, 512)
        self.assertEqual(response[48:64], plain[80:96])
        return response

    def test_apply_confirm_and_commit(self):
        self.exchange(report())
        rev = self.registry.queue(MAC, "apply", host="r.limanowa.org", port=54322)
        command = self.exchange(report())
        self.assertEqual(command[4], 1)
        operation = command[32:48]
        response = self.exchange(report(2, rev, pending=rev, phase=3, result=3, operation=operation, result_rev=rev, flags=1))
        self.assertEqual(response[4], 0)
        response = self.exchange(report(2, rev, pending=rev, phase=3, result=3, operation=operation, result_rev=rev))
        self.assertEqual(response[4], 6)
        self.exchange(report(3, rev, active=rev, result=4, operation=operation, result_rev=rev))
        self.assertIsNone(self.registry.status(MAC)[0]["queued_action"])

    def test_staged_armed_automatic_trial(self):
        self.exchange(report())
        first = self.registry.queue(MAC, "stage", ssid="future-network", password="test-password")
        operation = self.exchange(report())[32:48]
        self.exchange(report(2, first, pending=first, phase=1, result=1, operation=operation, result_rev=first))
        rev = self.registry.queue(MAC, "arm")
        operation = self.exchange(report(2, first, pending=first, phase=1, result=1, operation=operation, result_rev=first))[32:48]
        self.exchange(report(3, rev, pending=rev, phase=2, result=2, operation=operation, result_rev=rev))
        self.assertEqual(self.registry.status(MAC)[0]["queued_action"], "arm")
        self.assertEqual(self.exchange(report(4, rev, pending=rev, phase=3, result=3, operation=operation, result_rev=rev))[4], 6)

    def test_replay_conflict_and_reset(self):
        plain = report(4, 3, active=3)
        self.exchange(plain)
        with self.assertRaises(ValueError):
            self.exchange(plain)
        with self.assertRaises(ValueError):
            self.exchange(report(3, 3, active=3))
        with self.assertRaises(ValueError):
            self.exchange(report(4, 2, active=2))
        self.key = self.registry.enroll(MAC, rotate=True)
        self.exchange(report())  # Explicit new key starts a new enrollment epoch.

    def test_cancel_inflight_and_reject(self):
        self.exchange(report())
        apply = self.registry.queue(MAC, "apply", ssid="new", password="test-password")
        cancel = self.registry.queue(MAC, "cancel")
        command = self.exchange(report())
        self.assertEqual(command[4], 5)
        self.assertEqual(struct.unpack_from("<Q", command, 24)[0], apply)
        self.exchange(report(2, cancel, result=6, error=3, result_rev=cancel, operation=command[32:48]))
        self.assertIsNone(self.registry.status(MAC)[0]["queued_action"])

    def test_private_registry_and_encrypted_pending(self):
        self.exchange(report())
        self.registry.queue(MAC, "apply", ssid="test", password="never-plain-fixture")
        self.assertNotIn(b"never-plain-fixture", self.registry.path.read_bytes())
        self.assertEqual(self.registry.path.stat().st_mode & 0o777, 0o600)
        public = self.root / "open"
        public.mkdir()
        public.chmod(0o755)
        with self.assertRaises(ValueError):
            Registry(public / "public.sqlite")

    def test_enrollment_transfer_preserves_existing_server_state(self):
        self.exchange(report())
        self.registry.queue(MAC,"apply",host="example.org",port=54321)
        secret = self.registry.path.parent / "enrollment.json"
        self.registry.export_enrollment(MAC,secret)
        self.assertEqual(secret.stat().st_mode & 0o777,0o600)
        self.registry.import_enrollment(secret)
        self.assertEqual(self.registry.status(MAC)[0]["queued_action"],"apply")
        other = Registry(self.root / "server-private" / "devices.sqlite")
        other.import_enrollment(secret)
        self.assertEqual(other.key(MAC),self.key)
        other.enroll(MAC,rotate=True)
        with self.assertRaises(ValueError):
            other.import_enrollment(secret)
        other.import_enrollment(secret,replace=True)
        self.assertEqual(other.key(MAC),self.key)
        archive = package(self.root / "server.tar.gz")
        with tarfile.open(archive) as source:
            self.assertEqual(set(source.getnames()),{"goldrop_env.py","goldrop-server.sh","goldrop-client.sh","manage_devices.py","management_protocol.py","management_registry.py","provision_wifi.py","requirements-management.txt","device_identity.py","firmware_catalog.py","fleet_management.py","measurement_store.py","telemetry_protocol.py","serve_measurements.py","web/index.html","web/app.js","web/management.js","web/styles.css"})

    def test_usb_enrollment_no_secret_output(self):
        class Usb:
            key = None
            def __init__(self,port):
                self.port = port
            def command(self,line,expected):
                if line == "manage status":
                    return "CFG MANAGE mac="+MAC+" enrolled="+("true" if self.key else "false")+" key_id="+(key_id(self.key) if self.key else "none")
                if line.startswith("manage key "):
                    self.key=bytes.fromhex(line[11:])
                return expected
            def close(self):
                pass
        output=io.StringIO()
        local_db=self.root / "usb-private" / "devices.sqlite"
        with patch.object(provision_management,"UsbSession",Usb),patch.object(sys,"argv",["provision_management.py","--port","fake","--db",str(local_db),"--url","http://r.limanowa.org:8000"]),patch("sys.stdout",output):
            self.assertEqual(provision_management.main(),0)
        registered=Registry(local_db).key(MAC)
        self.assertNotIn(registered.hex(),output.getvalue())

    def test_hundred_device_registry_http_requests(self):
        public=self.root / "ota"
        public.mkdir()
        devices=[f"02:00:00:00:00:{i:02x}" for i in range(100)]
        keys={mac:self.registry.enroll(mac) for mac in devices}
        server=ManagementHTTPServer(("127.0.0.1",0),self.registry,public)
        worker=threading.Thread(target=server.serve_forever,daemon=True)
        worker.start()
        def exchange(mac):
            conn=http.client.HTTPConnection("127.0.0.1",server.server_port,timeout=5)
            try:
                plain=report()
                conn.request("POST","/management/v1/"+mac,body=seal(keys[mac],mac,b"C",plain))
                result=conn.getresponse()
                self.assertEqual(result.status,200)
                response=open_envelope(keys[mac],mac,b"S",result.read(),512)
                self.assertEqual(response[48:64],plain[80:96])
            finally:
                conn.close()
        try:
            with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
                list(pool.map(exchange,devices))
            self.assertEqual(sum(bool(d["report"]) for d in self.registry.status()),100)
        finally:
            server.shutdown();server.server_close();worker.join()

    def test_http_and_ota_and_limits(self):
        public = self.root / "ota"
        public.mkdir()
        (public / "manifest.json").write_text('{"fixture":true}')
        (public / "goldrop-fixture.bin").write_bytes(b"firmware")
        (public / "secrets.txt").write_text("private")
        (public / "goldrop-secret.bin").symlink_to(self.registry.path)
        server = ManagementHTTPServer(("127.0.0.1", 0), self.registry, public)
        worker = threading.Thread(target=server.serve_forever, daemon=True)
        worker.start()
        def request(method, path, data=None):
            conn = http.client.HTTPConnection("127.0.0.1", server.server_port, timeout=3)
            try:
                conn.request(method, path, body=data)
                response = conn.getresponse()
                return response.status, response.read()
            finally:
                conn.close()
        try:
            status, body = request("POST", "/management/v1/" + MAC, seal(self.key, MAC, b"C", report()))
            self.assertEqual(status, 200)
            self.assertEqual(len(open_envelope(self.key, MAC, b"S", body, 512)), 512)
            self.assertEqual(request("POST", "/management/v1/" + MAC, b"x" * 168)[0], 403)
            self.assertEqual(request("POST", "/management/v1/" + MAC, b"short")[0], 400)
            self.assertEqual(request("GET", "/manifest.json"), (200, b'{"fixture":true}'))
            self.assertEqual(request("GET", "/goldrop-fixture.bin"), (200, b"firmware"))
            for path in ("/", "/secrets.txt", "/../private/devices.sqlite", "/%2e%2e/private/devices.sqlite", "/goldrop-secret.bin", "/manifest.json?x=1"):
                self.assertEqual(request("GET", path)[0], 404)
        finally:
            server.shutdown()
            server.server_close()
            worker.join()

    def test_manifest_replacement_keeps_response_consistent(self):
        public=self.root / "ota"
        public.mkdir()
        path=public / "manifest.json"
        old=b'{"old":1}'
        path.write_bytes(old)
        pending=public / "manifest.json.new"
        pending.write_bytes(b'{"new_release_has_a_different_length":true}')
        server=ManagementHTTPServer(("127.0.0.1",0),self.registry,public)
        worker=threading.Thread(target=server.serve_forever,daemon=True)
        worker.start()
        real_open=Path.open
        def replace_after_open(target,*args,**kwargs):
            stream=real_open(target,*args,**kwargs)
            if target==path and pending.exists():
                os.replace(pending,path)
            return stream
        conn=http.client.HTTPConnection("127.0.0.1",server.server_port,timeout=2)
        try:
            with patch.object(Path,"open",replace_after_open):
                conn.request("GET","/manifest.json")
                response=conn.getresponse()
                self.assertEqual(response.status,200)
                self.assertEqual(int(response.getheader("Content-Length")),len(old))
                self.assertEqual(response.read(),old)
        finally:
            conn.close();server.shutdown();server.server_close();worker.join()

    def test_slow_headers_have_total_deadline(self):
        public=self.root / "ota"
        public.mkdir()
        server=ManagementHTTPServer(("127.0.0.1",0),self.registry,public)
        server.request_read_timeout=.15
        worker=threading.Thread(target=server.serve_forever,daemon=True)
        worker.start()
        client=socket.create_connection(("127.0.0.1",server.server_port),timeout=1)
        client.settimeout(.3)
        try:
            client.sendall(b"GET /manifest.json HTTP/1.1\r\nX-Slow: ")
            # Each byte arrives before the socket's per-read timeout.
            for _ in range(10):
                try:
                    client.sendall(b"x")
                except OSError:
                    break
                time.sleep(.025)
            self.assertEqual(client.recv(1),b"")
        finally:
            client.close();server.shutdown();server.server_close();worker.join()


if __name__ == "__main__":
    unittest.main()
