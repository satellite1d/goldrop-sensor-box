"""Private SQLite registry. CLI and HTTP handlers share transactional state."""

import json
import os
from pathlib import Path
import sqlite3
import stat
import time
from contextlib import contextmanager
from management_protocol import (
    MAX_REVISION,
    canonical_mac,
    key_id,
    seal,
    open_envelope,
    parse_report,
    report_identity,
    make_command,
    bind_challenge,
)


class Registry:
    def __init__(self, path):
        self.path = Path(path).expanduser().absolute()
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        if self.path.parent.stat().st_mode & 0o077:
            raise ValueError(
                "Katalog rejestru musi miec uprawnienia 700 (prywatny katalog poza OTA)"
            )
        if self.path.is_symlink():
            raise ValueError("Rejestr nie moze byc dowiazaniem")
        fd = os.open(self.path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        os.close(fd)
        if stat.S_IMODE(self.path.stat().st_mode) & 0o077:
            raise ValueError("Plik rejestru musi miec uprawnienia 600")
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            version = db.execute("PRAGMA user_version").fetchone()[0]
            if version not in (0, 1) or (
                version == 0
                and db.execute("SELECT 1 FROM sqlite_master WHERE type='table'").fetchone()
            ):
                raise ValueError("Nieznany format rejestru; plik zachowany")
            if version == 1:
                expected = {
                    "mac",
                    "key",
                    "counter",
                    "report",
                    "seen",
                    "command",
                    "command_revision",
                    "command_action",
                }
                columns = {r[1] for r in db.execute("PRAGMA table_info(devices)")}
                if columns != expected or db.execute("PRAGMA application_id").fetchone()[0] != 0:
                    raise ValueError("To nie jest rejestr zarzadzania; plik zachowany")
            db.execute(
                """CREATE TABLE IF NOT EXISTS devices (
                mac TEXT PRIMARY KEY, key BLOB NOT NULL, counter INTEGER NOT NULL DEFAULT 0,
                report BLOB, seen INTEGER, command BLOB, command_revision INTEGER, command_action TEXT)"""
            )
            db.execute(
                "CREATE TABLE IF NOT EXISTS ota_targets(mac TEXT PRIMARY KEY, manifest TEXT NOT NULL)"
            )
            db.execute("PRAGMA user_version=1")

    @contextmanager
    def connect(self):
        db = sqlite3.connect(self.path, timeout=5)
        try:
            db.row_factory = sqlite3.Row
            # Small infrequent writes; rollback journal, full fsync, no WAL sidecar exposure.
            db.execute("PRAGMA synchronous=FULL")
            with db:
                yield db
        finally:
            db.close()

    def key(self, mac):
        with self.connect() as db:
            row = db.execute(
                "SELECT key FROM devices WHERE mac=?", (canonical_mac(mac),)
            ).fetchone()
            if row is None:
                raise ValueError("Nieznane urzadzenie")
            return bytes(row["key"])

    def enroll(self, mac, rotate=False):
        mac = canonical_mac(mac)
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT key FROM devices WHERE mac=?", (mac,)).fetchone()
            if row is not None and not rotate:
                return bytes(row["key"])
            key = os.urandom(32)
            db.execute("INSERT OR REPLACE INTO devices(mac,key) VALUES(?,?)", (mac, key))
            return key

    def export_enrollment(self, mac, output):
        mac = canonical_mac(mac)
        # Explicit private artifact, never stdout and never part of an OTA package.
        target = Path(output).expanduser()
        if target.parent.stat().st_mode & 0o077:
            raise ValueError("Eksport zapisz w prywatnym katalogu 700, poza OTA")
        payload = json.dumps(dict(format=1, mac=mac, key=self.key(mac).hex())).encode("ascii")
        fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        with os.fdopen(fd, "wb") as stream:
            stream.write(payload)

    def import_enrollment(self, source, replace=False):
        path = Path(source).expanduser()
        if path.is_symlink() or path.stat().st_mode & 0o077 or path.stat().st_size > 1024:
            raise ValueError("Plik rejestracji musi byc prywatnym plikiem 600 do 1 kB")
        data = json.loads(path.read_text())
        if set(data) != {"format", "mac", "key"} or data["format"] != 1:
            raise ValueError("Nieprawidlowy format rejestracji")
        mac = canonical_mac(data["mac"])
        key = bytes.fromhex(data["key"])
        if len(key) != 32 or not any(key):
            raise ValueError("Nieprawidlowy klucz rejestracji")
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT key FROM devices WHERE mac=?", (mac,)).fetchone()
            if row and bytes(row["key"]) == key:
                return mac  # Preserve server counters, reports and queued operations.
            if row and not replace:
                raise ValueError("Inny klucz urzadzenia; po jawnej rotacji USB uzyj --replace")
            db.execute("INSERT OR REPLACE INTO devices(mac,key) VALUES(?,?)", (mac, key))
        return mac

    def status(self, mac=None):
        with self.connect() as db:
            rows = db.execute(
                "SELECT * FROM devices" + (" WHERE mac=?" if mac else "") + " ORDER BY mac",
                (canonical_mac(mac),) if mac else (),
            ).fetchall()
        return [
            dict(
                mac=r["mac"],
                key_id=key_id(r["key"]),
                last_seen=r["seen"],
                report=parse_report(bytes(r["report"])) if r["report"] else None,
                queued_action=r["command_action"],
                queued_revision=r["command_revision"],
            )
            for r in rows
        ]

    @staticmethod
    def supports_runtime(report):
        return report is not None and tuple(map(int, report["firmware"].split("."))) >= (0, 6, 0)

    def ota_targets(self):
        with self.connect() as db:
            return dict(db.execute("SELECT mac,manifest FROM ota_targets").fetchall())

    def ota_target(self, mac):
        with self.connect() as db:
            row = db.execute(
                "SELECT manifest FROM ota_targets WHERE mac=?", (canonical_mac(mac),)
            ).fetchone()
            return row[0] if row else None

    def queue(self, mac, action, **fields):
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            return self._queue(db, canonical_mac(mac), action, fields)

    def _queue(self, db, mac, action, fields):
        row = db.execute("SELECT * FROM devices WHERE mac=?", (mac,)).fetchone()
        if row is None or row["report"] is None:
            raise ValueError("Poczekaj na pierwszy uwierzytelniony raport plytki")
        report = parse_report(bytes(row["report"]))
        standalone = action in ("sampling", "ota_check")
        if standalone and not self.supports_runtime(report):
            raise ValueError("To polecenie wymaga firmware 0.6.0 lub nowszego")
        acknowledged_arm = (
            row["command_action"] == "arm"
            and report["phase"] == "armed"
            and report["pending_revision"] == row["command_revision"]
        )
        if (
            row["command"]
            and action != "cancel"
            and not (acknowledged_arm and action == "activate")
        ):
            raise ValueError("Poprzednia operacja oczekuje na wynik; uzyj status lub cancel")
        if (action in ("apply", "stage") or standalone) and report["phase"] != "idle":
            raise ValueError("Plytka ma juz kandydata; aktywuj lub anuluj")
        if action in ("activate", "arm") and report["phase"] not in ("staged", "armed"):
            raise ValueError("Najpierw przygotuj konfiguracje poleceniem stage")
        if action == "cancel" and report["phase"] == "idle" and not row["command"]:
            return None
        revision = max(row["counter"], report["highwater"]) + 1
        if revision > MAX_REVISION:
            raise ValueError("Wyczerpano numery rewizji; wymagana ponowna rejestracja przez USB")
        target = report["pending_revision"]
        if action == "cancel" and not target:
            target = row["command_revision"]
        command = make_command(
            action,
            revision,
            report["active_revision"],
            target if action not in ("apply", "stage", "sampling", "ota_check") else 0,
            os.urandom(16),
            **fields
        )
        encrypted = seal(bytes(row["key"]), mac, b"S", command)
        db.execute(
            "UPDATE devices SET counter=?,command=?,command_revision=?,command_action=? WHERE mac=?",
            (revision, encrypted, revision, action, mac),
        )
        return revision

    def queue_many(self, macs, action, manifest=None, already_current=(), **fields):
        macs = [canonical_mac(mac) for mac in macs]
        if not 1 <= len(macs) <= 128 or len(set(macs)) != len(macs):
            raise ValueError("Wybierz od 1 do 128 roznych plytek")
        results = []
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            for mac in macs:
                row = db.execute(
                    "SELECT report,command FROM devices WHERE mac=?", (mac,)
                ).fetchone()
                report = parse_report(bytes(row["report"])) if row and row["report"] else None
                if action == "ota_check" and manifest:
                    if (row and row["command"]) or (report and report["phase"] != "idle"):
                        raise ValueError("Najpierw zakoncz oczekujaca operacje konfiguracji")
                    db.execute(
                        "INSERT INTO ota_targets VALUES(?,?) ON CONFLICT(mac) DO UPDATE SET manifest=excluded.manifest",
                        (mac, manifest),
                    )
                    if mac in already_current:
                        results.append(dict(mac=mac, revision=None, mode="already_current"))
                        continue
                    if not self.supports_runtime(report):
                        results.append(dict(mac=mac, revision=None, mode="automatic_poll"))
                        continue
                revision = self._queue(db, mac, action, fields)
                results.append(dict(mac=mac, revision=revision, mode="command"))
        return results

    def exchange(self, mac, envelope):
        mac = canonical_mac(mac)
        key = self.key(mac)
        data = open_envelope(key, mac, b"C", envelope, 136)
        report = parse_report(data)
        response = make_command("none")
        with self.connect() as db:
            db.execute("BEGIN IMMEDIATE")
            row = db.execute("SELECT * FROM devices WHERE mac=?", (mac,)).fetchone()
            if row is None or bytes(row["key"]) != key:
                raise ValueError("Enrollment changed")
            if row["report"] is not None:
                old = bytes(row["report"])
                previous = parse_report(old)
                if report["sequence"] < previous["sequence"] or (
                    report["sequence"] == previous["sequence"]
                    and report_identity(old) != report_identity(data)
                ):
                    raise ValueError("Stale or conflicting report")
                if (
                    report["sequence"] == previous["sequence"]
                    and report["exchange_sequence"] <= previous["exchange_sequence"]
                ):
                    raise ValueError("Replayed exchange")
                if report["highwater"] < previous["highwater"]:
                    raise ValueError("Configuration counter decreased")
            db.execute(
                "UPDATE devices SET report=?,seen=?,counter=MAX(counter,?) WHERE mac=?",
                (data, int(time.time()), report["highwater"], mac),
            )
            if row["command"]:
                command = open_envelope(key, mac, b"S", bytes(row["command"]), 512)
                revision = row["command_revision"]
                operation = command[32:48].hex()
                if report["highwater"] < revision:
                    response = command
                elif report["pending_revision"] == revision and report["operation"] == operation:
                    if report["phase"] == "testing" and report["wifi"] and report["probe"]:
                        response = make_command(
                            "confirm",
                            revision,
                            report["active_revision"],
                            revision,
                            bytes.fromhex(operation),
                        )
                    elif report["phase"] in ("staged", "armed"):
                        # Keep the arm command: it must confirm the automatic trial later.
                        if report["phase"] == "staged":
                            db.execute(
                                "UPDATE devices SET command=NULL,command_revision=NULL,command_action=NULL WHERE mac=?",
                                (mac,),
                            )
                else:
                    db.execute(
                        "UPDATE devices SET command=NULL,command_revision=NULL,command_action=NULL WHERE mac=?",
                        (mac,),
                    )
        return seal(key, mac, b"S", bind_challenge(response, data[80:96]))
