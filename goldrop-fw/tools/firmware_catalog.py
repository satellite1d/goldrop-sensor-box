"""Read-only catalog of signed OTA releases prepared on the publisher's computer."""

import hashlib
import json
from pathlib import Path
import re
from urllib.parse import urlsplit
from device_identity import canonical_mac
from telemetry_protocol import unique_object


def document(raw):
    try:
        return json.loads(raw, object_pairs_hook=unique_object)
    except RecursionError as error:
        raise ValueError("Za gleboki dokument JSON") from error


VERSION = r"(?:0|[1-9][0-9]{0,8})\.(?:0|[1-9][0-9]{0,8})\.(?:0|[1-9][0-9]{0,8})"


class FirmwareCatalog:
    def __init__(self, directory):
        self.root = Path(directory).resolve(strict=True)
        if not self.root.is_dir():
            raise ValueError("Brak katalogu OTA")

    def read(self, filename, device=None):
        if not isinstance(filename, str) or not re.fullmatch(
            r"manifest-(" + VERSION + r")\.json", filename
        ):
            raise ValueError("Wybierz wersjonowany manifest wydania")
        path = self.root / filename
        if path.is_symlink():
            raise ValueError("Manifest nie moze byc dowiazaniem")
        with path.open("rb") as source:
            raw = source.read(8193)
        if len(raw) > 8192:
            raise ValueError("Za duzy manifest")
        envelope = document(raw)
        if (
            not isinstance(envelope, dict)
            or type(envelope.get("signature_schema")) is not int
            or envelope.get("signature_schema") != 1
            or envelope.get("algorithm") != "ecdsa-p256-sha256"
            or not isinstance(envelope.get("signed_manifest"), str)
        ):
            raise ValueError("Wymagany podpisany manifest")
        if set(envelope) != {
            "signature_schema",
            "algorithm",
            "signed_manifest",
            "signature_hex",
        } or not isinstance(envelope["signature_hex"], str):
            raise ValueError("Nieprawidlowa otoczka podpisu")
        signature = bytes.fromhex(envelope["signature_hex"])
        if not 8 <= len(signature) <= 72:
            raise ValueError("Nieprawidlowy podpis manifestu")
        manifest = document(envelope["signed_manifest"])
        if not isinstance(manifest, dict):
            raise ValueError("Nieprawidlowe metadane manifestu")
        for field in ("firmware_version", "hardware_revision", "binary_url", "channel", "sha256"):
            if not isinstance(manifest.get(field), str):
                raise ValueError("Nieprawidlowe metadane manifestu")
        if not re.fullmatch(r"[0-9a-fA-F]{64}", manifest["sha256"]) or (
            "mandatory" in manifest and type(manifest["mandatory"]) is not bool
        ):
            raise ValueError("Nieprawidlowy hash lub flaga wydania")
        version = manifest["firmware_version"]
        if (
            not isinstance(version, str)
            or not re.fullmatch(VERSION, version)
            or filename != f"manifest-{version}.json"
        ):
            raise ValueError("Nazwa manifestu nie zgadza sie z wersja")
        if (
            type(manifest.get("schema_version")) is not int
            or manifest.get("schema_version") != 1
            or not re.fullmatch(r"[a-z0-9_.-]{1,31}", manifest["hardware_revision"])
        ):
            raise ValueError("Nieprawidlowy sprzet lub schemat")
        url = urlsplit(manifest["binary_url"])
        binary = f"{manifest['hardware_revision']}-{version}.bin"
        if (
            url.scheme not in ("http", "https")
            or not url.hostname
            or url.username
            or url.password
            or url.query
            or url.fragment
            or url.path != "/" + binary
        ):
            raise ValueError("Nieprawidlowy URL obrazu")
        if not re.fullmatch(r"[a-z0-9_-]{1,16}", manifest["channel"]):
            raise ValueError("Nieprawidlowy kanal")
        targets = manifest.get("target_device_ids")
        if targets is not None and (not isinstance(targets, list) or len(targets) > 32):
            raise ValueError("Nieprawidlowa grupa wydania")
        targets = [canonical_mac(mac) for mac in targets] if targets is not None else None
        if device and targets is not None and canonical_mac(device) not in targets:
            raise ValueError("Podpisany manifest nie obejmuje tej plytki")
        image = self.root / binary
        if (
            image.is_symlink()
            or type(manifest["size_bytes"]) is not int
            or not 368 <= manifest["size_bytes"] <= 1572864
        ):
            raise ValueError("Nieprawidlowy obraz")
        if image.stat().st_size != manifest["size_bytes"]:
            raise ValueError("Rozmiar obrazu nie zgadza sie z manifestem")
        digest = hashlib.sha256()
        with image.open("rb") as source:
            for chunk in iter(lambda: source.read(65536), b""):
                digest.update(chunk)
        if digest.hexdigest() != manifest["sha256"].lower():
            raise ValueError("SHA-256 obrazu nie zgadza sie z manifestem")
        return (
            dict(
                id=filename,
                version=version,
                hardware_revision=manifest["hardware_revision"],
                channel=manifest["channel"],
                size_bytes=manifest["size_bytes"],
                target_device_ids=targets,
            ),
            raw,
        )

    def available(self):
        result = []
        paths = [
            p
            for p in self.root.glob("manifest-*.json")
            if re.fullmatch(r"manifest-(" + VERSION + r")\.json", p.name)
        ]
        paths.sort(
            key=lambda p: tuple(map(int, p.stem.removeprefix("manifest-").split("."))), reverse=True
        )
        for path in paths[:100]:
            try:
                result.append(self.read(path.name)[0])
            except (ValueError, KeyError, TypeError, OSError):
                continue
        return sorted(result, key=lambda r: tuple(map(int, r["version"].split("."))), reverse=True)
