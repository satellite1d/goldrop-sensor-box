"""Dashboard facade: public status only, encrypted commands stay in the private registry."""

from pathlib import Path
from device_identity import canonical_mac
from firmware_catalog import FirmwareCatalog
from management_registry import Registry


class FleetManagement:
    def __init__(self, database, ota_directory, measurements):
        if not Path(database).expanduser().is_file():
            raise ValueError("Najpierw przygotuj prywatny rejestr zarzadzania")
        self.registry = Registry(database)
        self.catalog = FirmwareCatalog(ota_directory)
        self.measurements = measurements
        if self.catalog.root in self.registry.path.resolve().parents:
            raise ValueError("Rejestr kluczy musi byc poza katalogiem OTA")

    def snapshot(self, include_releases=True):
        telemetry = {d["device_id"]: d for d in self.measurements.devices()}
        enrolled = {r["mac"]: r for r in self.registry.status()}
        targets = self.registry.ota_targets()
        devices = []
        for mac in sorted(telemetry.keys() | enrolled.keys()):
            row = enrolled.get(mac, {})
            report = row.get("report")
            device = telemetry.get(mac, {})
            status = device.get("status", {})
            target = targets.get(mac)
            devices.append(
                dict(
                    device_id=mac,
                    name=device.get("name", ""),
                    enrolled=mac in enrolled,
                    firmware_version=status.get(
                        "firmware_version", report["firmware"] if report else None
                    ),
                    hardware_revision=status.get("hardware_revision"),
                    interval_ms=status.get("sampling_interval_ms"),
                    last_seen_ms=device.get("last_seen_ms"),
                    management_last_seen=row.get("last_seen"),
                    report=report,
                    queued_action=row.get("queued_action"),
                    queued_revision=row.get("queued_revision"),
                    supports_runtime=self.registry.supports_runtime(report),
                    ota_manifest=target,
                    ota_state=status.get("ota_state"),
                    ota_progress=status.get("ota_progress"),
                    ota_error=status.get("ota_error"),
                )
            )
        return dict(
            enabled=True,
            devices=devices,
            releases=self.catalog.available() if include_releases else [],
        )

    def command(self, body):
        if not isinstance(body, dict):
            raise ValueError("Nieprawidlowe zadanie")
        action = body.get("action")
        allowed = {
            "apply": {"ssid", "password"},
            "stage": {"ssid", "password"},
            "activate": set(),
            "arm": set(),
            "cancel": set(),
            "sampling": {"interval_seconds"},
            "ota_check": {"release", "confirm_buffer_loss"},
        }
        if action not in allowed or set(body) != {"action", "devices"} | allowed[action]:
            raise ValueError("Nieprawidlowe pola polecenia")
        if not isinstance(body["devices"], list) or not 1 <= len(body["devices"]) <= 128:
            raise ValueError("Wybierz od 1 do 128 plytek")
        macs = [canonical_mac(mac) for mac in body["devices"]]
        if len(set(macs)) != len(macs):
            raise ValueError("Powtorzony MAC")
        known = {d["device_id"]: d for d in self.snapshot(include_releases=False)["devices"]}
        if any(mac not in known for mac in macs):
            raise ValueError("Nieznana plytka")
        fields = {}
        manifest = None
        skipped = []
        if action in ("apply", "stage"):
            if not isinstance(body["ssid"], str) or not isinstance(body["password"], str):
                raise ValueError("Podaj SSID i haslo")
            fields = dict(ssid=body["ssid"], password=body["password"])
        elif action == "sampling":
            seconds = body["interval_seconds"]
            if type(seconds) is not int or not 10 <= seconds <= 3600:
                raise ValueError("Interwal musi wynosic od 10 do 3600 sekund")
            fields = dict(interval_ms=seconds * 1000)
        elif action == "ota_check":
            if body["confirm_buffer_loss"] is not True:
                raise ValueError("Potwierdz usuniecie bufora przy aktualizacji")
            release, _ = self.catalog.read(body["release"])
            manifest = release["id"]
            for mac in macs:
                d = known[mac]
                if d["hardware_revision"] != release["hardware_revision"]:
                    raise ValueError("Wybrane wydanie nie pasuje do sprzetu jednej z plytek")
                targets = release["target_device_ids"]
                if targets is not None and mac not in targets:
                    raise ValueError("Podpisany manifest nie obejmuje wszystkich wybranych plytek")
                current = d["firmware_version"]
                if not current:
                    raise ValueError("Poczekaj na raport wersji firmware")
                current_version = tuple(map(int, current.split(".")))
                new_version = tuple(map(int, release["version"].split(".")))
                if current_version > new_version:
                    raise ValueError("Nie mozna instalowac starszego firmware")
                if current_version == new_version:
                    skipped.append(mac)
        results = self.registry.queue_many(
            macs, action, manifest=manifest, already_current=skipped, **fields
        )
        return dict(results=results)
