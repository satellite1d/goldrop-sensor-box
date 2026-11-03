#!/usr/bin/env python3
"""Package an ESP32-C3 application and derive release metadata from its embedded descriptors."""
import argparse
import hashlib
import ipaddress
import json
import os
from pathlib import Path
import re
import struct
import sys
import subprocess
import tempfile
from urllib.parse import urlsplit

SLOT_SIZE = 0x180000
CUSTOM_OFFSET = 288
PREFIX_SIZE = 368


def publish_complete(path, data, immutable=False):
    """Publish a complete file; preserve the previous release on failed writes."""
    descriptor, name = tempfile.mkstemp(prefix=".goldrop-", suffix=".tmp", dir=path.parent)
    temporary = Path(name)
    try:
        with os.fdopen(descriptor, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        # Release files are public; retain readability for a separate HTTP user.
        temporary.chmod(0o644)
        if immutable:
            try:
                os.link(temporary, path)  # Atomic create; never overwrite an existing release.
            except FileExistsError:
                if path.read_bytes() != data:
                    raise ValueError("This release name already contains another image; increment version.txt")
        else:
            os.replace(temporary, path)
    finally:
        temporary.unlink(missing_ok=True)


def text_field(data):
    if b"\0" not in data:
        raise ValueError("Unterminated image metadata")
    return data.split(b"\0", 1)[0].decode("ascii")


def inspect_image(data):
    if not PREFIX_SIZE <= len(data) <= SLOT_SIZE or data[0] != 0xE9 or struct.unpack_from("<H", data, 12)[0] != 5:
        raise ValueError("Expected an ESP32-C3 application fitting a 1.5 MiB OTA slot")
    if struct.unpack_from("<I", data, 32)[0] != 0xABCD5432 or text_field(data[80:112]) != "goldrop":
        raise ValueError("Expected Goldrop application, not bootloader/merged flash image")
    version = text_field(data[48:80])
    if not re.fullmatch(r"(?:0|[1-9][0-9]{0,8})\.(?:0|[1-9][0-9]{0,8})\.(?:0|[1-9][0-9]{0,8})", version):
        raise ValueError("Unsupported firmware version")
    custom = data[CUSTOM_OFFSET:PREFIX_SIZE]
    if custom[:8] != b"GDROPFW1" or struct.unpack_from("<I", custom, 8)[0] != 1 or struct.unpack_from("<I", custom, 76)[0] != 0:
        raise ValueError("Missing/unsupported Goldrop hardware descriptor")
    hardware = text_field(custom[12:44])
    if not re.fullmatch(r"[A-Za-z0-9._-]{1,31}", hardware) or text_field(custom[44:76]) != version:
        raise ValueError("Hardware or version descriptor mismatch")
    return version, hardware


def package(binary, output, base_url, channel="stable", targets=None, mandatory=False, signing_key=None):
    parsed = urlsplit(base_url)
    if not base_url.startswith(("https://", "http://")) or not parsed.hostname or parsed.netloc.endswith(":") or parsed.username or parsed.password or parsed.query or parsed.fragment or not base_url.isascii() or any(ord(c) < 33 or c in "\\@" for c in base_url):
        raise ValueError("Use an HTTP(S) publication base URL without credentials/query/fragment")
    if parsed.scheme == "http" and not signing_key:
        raise ValueError("HTTP OTA requires --sign-key; unsigned HTTP releases are not supported")
    host = parsed.hostname
    if len(host) > 253 or any(not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label) for label in host.split(".")):
        raise ValueError("Use a DNS hostname or IPv4 address supported by the device")
    if all(c in "0123456789." for c in host):
        address = ipaddress.IPv4Address(host)
        if int(address) >> 24 == 0 or int(address) >> 24 >= 224:
            raise ValueError("Unsupported HTTPS IPv4 address")
    if parsed.port is not None and not 1 <= parsed.port <= 65535:
        raise ValueError("Invalid HTTPS port")
    if not re.fullmatch(r"[a-z0-9_-]{1,16}", channel):
        raise ValueError("Invalid channel")
    normalized = []
    if targets is not None:
        if len(targets) > 32:
            raise ValueError("At most 32 pilot MAC addresses")
        for mac in targets:
            if not re.fullmatch(r"[0-9A-Fa-f]{2}(?::[0-9A-Fa-f]{2}){5}", mac):
                raise ValueError("Invalid pilot MAC")
            raw = bytes.fromhex(mac.replace(":", ""))
            if not any(raw) or raw[0] & 1:
                raise ValueError("Pilot MAC must identify a station")
            normalized.append(mac.lower())
    data = Path(binary).read_bytes()
    version, hardware = inspect_image(data)
    filename = f"{hardware}-{version}.bin"
    url = base_url.rstrip("/") + "/" + filename
    if len(url) >= 384:
        raise ValueError("Binary URL exceeds firmware limit")
    manifest = dict(schema_version=1, firmware_version=version, hardware_revision=hardware,
                    channel=channel, binary_url=url, size_bytes=len(data),
                    sha256=hashlib.sha256(data).hexdigest(), mandatory=mandatory)
    if targets is not None:
        manifest["target_device_ids"] = normalized
    document = manifest
    if signing_key:
        from ota_signing import public_key_pem, sign_payload
        public = public_key_pem(signing_key)
        if public not in data:
            raise ValueError("Image must embed the signing key's public key; run init_ota_keys.py and rebuild")
        signed = json.dumps(manifest, separators=(",", ":"), ensure_ascii=True)
        document = dict(signature_schema=1, algorithm="ecdsa-p256-sha256", signed_manifest=signed,
                        signature_hex=sign_payload(signed.encode("ascii"), signing_key).hex())
    payload = json.dumps(document, indent=2) + "\n"
    if len(payload.encode()) > 8192:
        raise ValueError("Manifest exceeds firmware limit")
    destination = Path(output)
    destination.mkdir(parents=True, exist_ok=True)
    artifact = destination / filename
    if artifact.exists() and artifact.read_bytes() != data:
        raise ValueError("This release name already contains another image; increment version.txt")
    if not artifact.exists():
        publish_complete(artifact, data, immutable=True)
    publish_complete(destination / "manifest.json", payload.encode("utf-8"))
    return manifest, artifact


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, default=Path(".pio/build/goldrop/firmware.bin"))
    parser.add_argument("--output", type=Path, default=Path("build/ota"))
    parser.add_argument("--base-url", required=True, help="e.g. https://r.limanowa.org/goldrop/ota")
    parser.add_argument("--channel", default="stable")
    parser.add_argument("--target", action="append", help="Pilot station MAC; repeat. Omit for all compatible devices")
    parser.add_argument("--mandatory", action="store_true")
    parser.add_argument("--sign-key", type=Path, help="ECDSA P-256 private key; required for HTTP")
    args = parser.parse_args()
    try:
        manifest, artifact = package(args.binary, args.output, args.base_url, args.channel, args.target, args.mandatory, args.sign_key)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(str(error), file=sys.stderr)
        return 1
    print(f"Image: {artifact} ({manifest['size_bytes']} bytes)")
    print(f"Manifest: {args.output / 'manifest.json'}")
    print(f"SHA-256: {manifest['sha256']}")
    print("Publish the complete binary first; atomically replace manifest.json last.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
