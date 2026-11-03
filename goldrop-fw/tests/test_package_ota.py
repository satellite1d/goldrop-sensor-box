import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
import hashlib
import io
import os
from concurrent.futures import ThreadPoolExecutor
from unittest.mock import patch
import subprocess
import sys
import functools
import http.server
import threading
import urllib.request
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from ota_signing import initialize_keys, public_key_pem

spec = importlib.util.spec_from_file_location("package_ota", Path(__file__).resolve().parents[1] / "tools/package_ota.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


def image():
    data = bytearray(4096)
    data[0] = 0xE9
    struct.pack_into("<H", data, 12, 5)
    struct.pack_into("<I", data, 32, 0xABCD5432)
    data[48:53] = b"0.2.1"
    data[80:87] = b"goldrop"
    data[288:296] = b"GDROPFW1"
    struct.pack_into("<I", data, 296, 1)
    data[300:314] = b"goldrop-c3-v1.0"
    data[332:337] = b"0.2.1"
    return data


class PackageTests(unittest.TestCase):
    def test_failed_writes_keep_last_manifest_and_allow_retry(self):
        for fail_at in (1,2):
            with self.subTest(fail_at=fail_at),tempfile.TemporaryDirectory() as directory:
                root=Path(directory)
                binary=root / "firmware.bin"
                binary.write_bytes(image())
                output=root / "out"
                output.mkdir()
                previous=b'{"previous":"manifest"}'
                (output / "manifest.json").write_bytes(previous)
                real_open=io.open
                writes=0
                class FailingWriter:
                    def __init__(self,stream):self.stream=stream
                    def __getattr__(self,name):return getattr(self.stream,name)
                    def __enter__(self):return self
                    def __exit__(self,*args):return self.stream.__exit__(*args)
                    def write(self,data):
                        nonlocal writes
                        writes+=1
                        if writes==fail_at:
                            self.stream.write(data[:len(data)//2])
                            self.stream.flush()
                            raise OSError("injected interrupted write")
                        return self.stream.write(data)
                def intercepted(file,mode="r",*args,**kwargs):
                    stream=real_open(file,mode,*args,**kwargs)
                    path=Path(os.readlink(f"/proc/self/fd/{file}")) if isinstance(file,int) else Path(file)
                    return FailingWriter(stream) if path.parent==output and any(c in mode for c in "wx") else stream
                with patch.object(io,"open",intercepted):
                    with self.assertRaises(OSError):
                        module.package(binary,output,"https://updates.example/ota")
                self.assertEqual((output / "manifest.json").read_bytes(),previous)
                self.assertFalse(any(p.suffix==".tmp" for p in output.iterdir()))
                artifact=output / "goldrop-c3-v1.0-0.2.1.bin"
                self.assertTrue(not artifact.exists() or artifact.read_bytes()==binary.read_bytes())
                module.package(binary,output,"https://updates.example/ota")

    def test_parallel_publication_is_atomic(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory)
            binary=root / "firmware.bin"
            binary.write_bytes(image())
            output=root / "out"
            with ThreadPoolExecutor(max_workers=8) as pool:
                releases=list(pool.map(lambda i:module.package(binary,output,"https://updates.example/ota",channel=f"test{i}")[0],range(8)))
            self.assertIn(json.loads((output / "manifest.json").read_text()),releases)
            self.assertFalse(any(p.suffix==".tmp" for p in output.iterdir()))
    def test_python_http_publication(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            private, public = root / "secrets/private.pem", root / "public.pem"
            initialize_keys(private, public)
            binary = root / "firmware.bin"
            binary.write_bytes(image() + public.read_bytes())
            output = root / "release"
            output.mkdir()
            class Handler(http.server.SimpleHTTPRequestHandler):
                def log_message(self, *args):
                    pass
            server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Handler, directory=str(output)))
            worker = threading.Thread(target=server.serve_forever, daemon=True)
            worker.start()
            try:
                base = f"http://127.0.0.1:{server.server_port}"
                manifest, _ = module.package(binary, output, base, signing_key=private)
                opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
                with opener.open(base + "/manifest.json", timeout=3) as response:
                    self.assertEqual(response.status, 200)
                    wire = response.read()
                    self.assertEqual(int(response.headers["Content-Length"]), len(wire))
                document = json.loads(wire)
                self.assertEqual(json.loads(document["signed_manifest"]), manifest)
                with opener.open(manifest["binary_url"], timeout=3) as response:
                    self.assertEqual(response.status, 200)
                    self.assertEqual(int(response.headers["Content-Length"]), manifest["size_bytes"])
                    self.assertEqual(hashlib.sha256(response.read()).hexdigest(), manifest["sha256"])
            finally:
                server.shutdown()
                worker.join(3)
                server.server_close()
    def test_signed_http_manifest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            private, public = root / "secrets/private.pem", root / "public.pem"
            fingerprint = initialize_keys(private, public)
            self.assertEqual(fingerprint, hashlib.sha256(public.read_bytes()).hexdigest())
            original_key = private.read_bytes()
            self.assertEqual(initialize_keys(private, public), fingerprint)
            self.assertEqual(private.read_bytes(), original_key)
            self.assertEqual(private.stat().st_mode & 0o777, 0o600)
            binary = root / "firmware.bin"
            binary.write_bytes(image() + public_key_pem(private))
            manifest, artifact = module.package(binary, root / "out", "http://updates.example:8000",
                                                signing_key=private)
            signed = json.loads((root / "out/manifest.json").read_text())
            self.assertEqual(json.loads(signed["signed_manifest"]), manifest)
            signature = root / "signature.der"
            signature.write_bytes(bytes.fromhex(signed["signature_hex"]))
            command = ["openssl", "dgst", "-sha256", "-verify", str(public), "-signature", str(signature)]
            verified = subprocess.run(command, input=signed["signed_manifest"].encode(), capture_output=True)
            self.assertEqual(verified.returncode, 0)
            tampered = signed["signed_manifest"].replace('"0.2.1"', '"0.2.2"')
            rejected = subprocess.run(command, input=tampered.encode(), capture_output=True)
            self.assertNotEqual(rejected.returncode, 0)
            self.assertNotIn(b"PRIVATE KEY", artifact.read_bytes())
            self.assertEqual({p.suffix for p in (root / "out").iterdir()}, {".json", ".bin"})
            wrong_private, wrong_public = root / "wrong/private.pem", root / "wrong/public.pem"
            initialize_keys(wrong_private, wrong_public)
            with self.assertRaises(ValueError):
                module.package(binary, root / "wrong-out", "http://updates.example:8000", signing_key=wrong_private)
            with self.assertRaises(ValueError):
                initialize_keys(wrong_private, public)
            missing_private = root / "lost/private.pem"
            with self.assertRaises(ValueError):
                initialize_keys(missing_private, public)
            self.assertFalse(missing_private.exists())
    def test_metadata_and_digest(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "firmware.bin"
            binary.write_bytes(image())
            manifest, artifact = module.package(binary, root / "out", "https://updates.example/ota", targets=["02:00:00:00:00:01"])
            self.assertEqual(manifest["firmware_version"], "0.2.1")
            self.assertEqual(manifest["hardware_revision"], "goldrop-c3-v1.0")
            self.assertEqual(manifest["sha256"], module.hashlib.sha256(binary.read_bytes()).hexdigest())
            self.assertEqual(json.loads((root / "out/manifest.json").read_text()), manifest)
            self.assertEqual(artifact.read_bytes(), binary.read_bytes())
            corrupted = image()
            corrupted[-1] = 1
            binary.write_bytes(corrupted)
            with self.assertRaises(ValueError):
                module.package(binary, root / "out", "https://updates.example/ota")

    def test_wrong_image_or_metadata(self):
        for offset in (0, 12, 32, 48, 80, 288, 296, 300, 332):
            data = image()
            data[offset] = 0 if offset == 300 else data[offset] ^ 1
            with self.assertRaises(ValueError):
                module.inspect_image(data)

    def test_https_and_targets(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            binary = root / "firmware.bin"
            binary.write_bytes(image())
            for url in ("http://updates.example/ota", "HTTPS://updates.example/ota", "https://user:pass@updates.example/ota", "https://updates.example/ota?token=x", "https://[::1]/ota", "https://bad_host/ota", "https://999.1.1.1/ota"):
                with self.assertRaises(ValueError):
                    module.package(binary, root / "out", url)
            with self.assertRaises(ValueError):
                module.package(binary, root / "out", "https://updates.example/ota", targets=["ff:ff:ff:ff:ff:ff"])


if __name__ == "__main__":
    unittest.main()
