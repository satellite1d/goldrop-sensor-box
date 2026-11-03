"""Host installer tests; only standard library, no real serial port or credentials."""
import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import types
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("installer", Path(__file__).resolve().parents[1] / "tools/provision_wifi.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)


class SerialDevice:
    def __init__(self, fail_save=False, old_firmware=False, signed_http=False):
        self.pending = bytearray()
        self.reply = b""
        self.commands = []
        self.closed = self.opened = self.wifi = self.server = False
        self.fail_save, self.old_firmware = fail_save, old_firmware
        self.ota_saved = False
        self.signed_http = signed_http

    def open(self):
        self.opened = True

    def close(self):
        self.closed = True

    def write(self, data):
        self.pending.extend(data)
        if self.pending.endswith(b"\n"):
            line = self.pending[:-1].decode()
            self.commands.append(line)
            self.pending.clear()
            if line == "wifi status":
                reply = "CFG STATUS state=" + ("online" if self.wifi else "unconfigured") + " ip=-"
            elif line == "server status":
                reply = "CFG ERROR INVALID" if self.old_firmware else "CFG SERVER state=" + ("connected" if self.wifi and self.server else "unconfigured") + " transport=tcp_test"
            elif line.startswith("server address "):
                reply = "CFG OK SERVER ADDRESS"
            elif line.startswith("server port "):
                reply = "CFG OK SERVER PORT"
            elif line == "server save":
                reply = "CFG ERROR STORAGE" if self.fail_save else "CFG OK SERVER SAVE"
                self.server = not self.fail_save
            elif line.startswith("wifi ssid "):
                reply = "CFG OK SSID"
            elif line.startswith("wifi password "):
                reply = "CFG OK PASSWORD"
            elif line == "wifi save":
                self.wifi = True
                reply = "CFG OK SAVE"
            elif line == "ota status":
                reply = "CFG OTA state=disabled progress=0 error=none firmware=0.3.0 hardware=goldrop-c3-v1.0 signed_http=" + ("true" if self.signed_http else "false")
            elif any(line.startswith(prefix) for prefix in ("ota url ", "ota channel ", "ota interval ", "ota enable ")):
                reply = "CFG OK OTA FIELD"
            elif line == "ota save":
                reply = "CFG ERROR STORAGE" if self.fail_save else "CFG OK OTA SAVE"
                self.ota_saved = not self.fail_save
            elif line in ("ota check", "ota retry", "ota disable", "ota forget"):
                reply = "CFG OK OTA " + line.split()[1].upper()
            else:
                raise AssertionError("Unexpected wire command")
            self.reply = ("\x1b[32mI (1) usb_config: " + reply + "\x1b[0m\r\n").encode()
        return len(data)

    def read_until(self, *args):
        reply, self.reply = self.reply, b""
        return reply


class InstallerTests(unittest.TestCase):
    def run_installer(self, device, address="192.168.1.50", port="9000", status=False, upload=None, upload_result=0, ota=None, ota_url="https://updates.example/manifest.json"):
        replies = iter([ota_url, "stable", "900", "n"] if ota else ["unit-test network", address, port])

        def prompt(_):
            self.assertEqual(device.commands, ["wifi status", "server status"] + (["ota status"] if ota else []))
            return next(replies)

        def upload_command(command, **kwargs):
            self.assertFalse(device.opened)
            self.assertFalse(device.commands)
            self.assertEqual(command, ["pio", "run", "-e", "goldrop", "-t", "upload", "--upload-port", "/fake"]
                             if upload == "pio" else ["idf.py", "-B", "build/native", "-p", "/fake", "flash"])
            return types.SimpleNamespace(returncode=upload_result)

        serial = types.SimpleNamespace(Serial=lambda **kwargs: device, SerialException=OSError)
        output = io.StringIO()
        arguments = ["installer", "--port", "/fake"] + (["--status"] if status else []) + (["--upload", upload] if upload else []) + (["--ota", ota] if ota else [])
        with patch.dict(sys.modules, serial=serial), patch.object(sys, "argv", arguments), patch("builtins.input", side_effect=prompt) as asked, patch.object(installer.getpass, "getpass", return_value="unit-test-password"), patch.object(installer.time, "sleep"), patch.object(installer.subprocess, "run", side_effect=upload_command) as uploaded, contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            result = installer.main()
        self.assertEqual(uploaded.call_count, int(upload is not None))
        self.assertTrue(device.closed)
        self.assertNotIn("unit-test-password", output.getvalue())
        self.assertNotIn("unit-test network", output.getvalue())
        return result, asked.call_count

    def test_configuration(self):
        device = SerialDevice()
        self.assertEqual(self.run_installer(device), (0, 3))
        self.assertIn("server address 192.168.1.50", device.commands)
        self.assertIn("server port 9000", device.commands)
        self.assertIn("server save", device.commands)
        self.assertTrue(device.wifi and device.server)

    def test_status_and_old_firmware(self):
        self.assertEqual(self.run_installer(SerialDevice(), status=True), (0, 0))
        self.assertEqual(self.run_installer(SerialDevice(old_firmware=True)), (1, 0))

    def test_invalid_endpoint_does_not_write(self):
        for address, port in [("https://example.org", "9000"), ("224.0.0.1", "9000"), ("192.168.1.50", "65536")]:
            device = SerialDevice()
            self.assertEqual(self.run_installer(device, address, port)[0], 1)
            self.assertEqual(device.commands, ["wifi status", "server status"])

    def test_storage_failure_does_not_write_wifi(self):
        device = SerialDevice(fail_save=True)
        self.assertEqual(self.run_installer(device)[0], 1)
        self.assertFalse(device.wifi or device.server)
        self.assertNotIn("wifi save", device.commands)

    def test_dns_and_upload(self):
        for builder in (None, "pio", "idf"):
            device = SerialDevice()
            self.assertEqual(self.run_installer(device, address="R.LIMANOWA.ORG", upload=builder), (0, 3))
            self.assertIn("server address r.limanowa.org", device.commands)

    def test_upload_failure_stops_before_configuration(self):
        device = SerialDevice()
        self.assertEqual(self.run_installer(device, upload="pio", upload_result=1), (1, 0))
        self.assertFalse(device.opened or device.commands)

    def test_hostname_validation(self):
        self.assertEqual(installer.normalize_server_host("R.LIMANOWA.ORG"), "r.limanowa.org")
        longest = ".".join(["a" * 63, "b" * 63, "c" * 63, "d" * 61])
        self.assertEqual(installer.normalize_server_host(longest), longest)
        for value in ("bad_name.org", "host:9000", "host/path", "user@host", "1.2.3.999", "01.2.3.4", "::1", "host.", "a" * 64, longest + "x", "zażółć.pl"):
            with self.assertRaises(ValueError):
                installer.normalize_server_host(value)

    def test_ota_configuration_and_controls(self):
        device = SerialDevice()
        self.assertEqual(self.run_installer(device, ota="configure"), (0, 4))
        self.assertTrue(device.ota_saved)
        self.assertIn("ota enable 0", device.commands)
        self.assertNotIn("wifi save", device.commands)
        for action in ("status", "check", "disable", "forget", "retry"):
            device = SerialDevice()
            self.assertEqual(self.run_installer(device, ota=action), (0, 0))
            self.assertIn("ota " + action, device.commands)

    def test_invalid_ota_url_and_storage_failure(self):
        for url in ("http://updates.example/manifest.json", "https://user:pass@updates.example/m.json", "https://updates.example/m.json#fragment"):
            device = SerialDevice()
            self.assertEqual(self.run_installer(device, ota="configure", ota_url=url)[0], 1)
            self.assertFalse(device.ota_saved)
            self.assertNotIn("ota save", device.commands)
        device = SerialDevice(fail_save=True)
        self.assertEqual(self.run_installer(device, ota="configure")[0], 1)
        self.assertFalse(device.ota_saved or device.wifi)

    def test_signed_http(self):
        device = SerialDevice(signed_http=True)
        url = "http://r.limanowa.org:8000/manifest.json"
        self.assertEqual(self.run_installer(device, ota="configure", ota_url=url), (0, 4))
        self.assertIn("ota url " + url, device.commands)
        old = SerialDevice()
        self.assertEqual(self.run_installer(old, ota="configure", ota_url=url)[0], 1)
        self.assertNotIn("ota save", old.commands)


if __name__ == "__main__":
    unittest.main()
