"""One private SQLite file; acknowledge only after a FULL synchronous commit."""

from contextlib import contextmanager
import json
import os
from pathlib import Path
import sqlite3
import time
from device_identity import canonical_mac
from telemetry_protocol import MEASUREMENT_FIELDS, integer, MAX_I64


class MeasurementStore:
    def __init__(self, path):
        self.path = Path(path).expanduser().absolute()
        self.path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        if self.path.parent.stat().st_mode & 0o077 or self.path.is_symlink():
            raise ValueError("Baza wymaga prywatnego katalogu 700 poza OTA")
        fd = os.open(self.path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
        os.close(fd)
        if self.path.stat().st_mode & 0o077:
            raise ValueError("Baza wymaga uprawnien 600")
        with self.connection() as db:
            version = db.execute("PRAGMA user_version").fetchone()[0]
            tables = db.execute("SELECT name FROM sqlite_master WHERE type='table'").fetchall()
            application = db.execute("PRAGMA application_id").fetchone()[0]
            if version not in (0, 1) or (tables and application != 0x47444D31):
                raise ValueError("Nieznany format bazy; plik zachowany")
            if db.execute("PRAGMA journal_mode=WAL").fetchone()[0] != "wal":
                raise ValueError("Nie mozna wlaczyc WAL")
            db.execute("BEGIN IMMEDIATE")
            db.execute("""CREATE TABLE IF NOT EXISTS devices(
                device_id TEXT PRIMARY KEY,name TEXT NOT NULL DEFAULT '',last_seen_ms INTEGER NOT NULL,
                latest_id INTEGER,status_json TEXT NOT NULL DEFAULT '{}')""")
            db.execute("""CREATE TABLE IF NOT EXISTS measurements(
                id INTEGER PRIMARY KEY,device_id TEXT NOT NULL,boot_id TEXT NOT NULL,sequence TEXT NOT NULL,
                uptime_ms INTEGER NOT NULL,timestamp_ms INTEGER,temperature_sht_c REAL,humidity_rh REAL,
                temperature_ds18_c REAL,sht45_valid INTEGER NOT NULL,ds18b20_valid INTEGER NOT NULL,
                sht45_error TEXT NOT NULL,ds18b20_error TEXT NOT NULL,received_ms INTEGER NOT NULL,effective_ms INTEGER NOT NULL,
                UNIQUE(device_id,boot_id,sequence))""")
            db.execute(
                "CREATE INDEX IF NOT EXISTS measurement_time ON measurements(device_id,effective_ms,id)"
            )
            db.execute("PRAGMA user_version=1")
            db.execute("PRAGMA application_id=1195658545")
            db.execute("COMMIT")

    @contextmanager
    def connection(self):
        db = sqlite3.connect(self.path, timeout=5, isolation_level=None)
        db.row_factory = sqlite3.Row
        try:
            db.execute("PRAGMA synchronous=FULL")
            yield db
        finally:
            if db.in_transaction:
                db.rollback()
            db.close()

    def save(self, message, now_ms=None):
        now = int(time.time() * 1000) if now_ms is None else now_ms
        with self.connection() as db:
            db.execute("BEGIN IMMEDIATE")
            mac = message["device_id"]
            db.execute(
                "INSERT INTO devices(device_id,last_seen_ms) VALUES(?,?) ON CONFLICT(device_id) DO UPDATE SET last_seen_ms=excluded.last_seen_ms",
                (mac, now),
            )
            if message["message_type"] == "status":
                db.execute(
                    "UPDATE devices SET status_json=? WHERE device_id=?",
                    (json.dumps(message, separators=(",", ":")), mac),
                )
                db.execute("COMMIT")
                return False
            values = tuple(message[field] for field in MEASUREMENT_FIELDS)
            columns = ",".join(MEASUREMENT_FIELDS)
            cursor = db.execute(
                f"INSERT INTO measurements({columns},received_ms,effective_ms) VALUES({','.join('?' for _ in values)},?,?) ON CONFLICT(device_id,boot_id,sequence) DO NOTHING",
                values
                + (now, message["timestamp_ms"] if message["timestamp_ms"] is not None else now),
            )
            inserted = cursor.rowcount == 1
            if inserted:
                db.execute(
                    "UPDATE devices SET latest_id=? WHERE device_id=?", (cursor.lastrowid, mac)
                )
            else:
                previous = db.execute(
                    f"SELECT {columns} FROM measurements WHERE device_id=? AND boot_id=? AND sequence=?",
                    (mac, message["boot_id"], message["sequence"]),
                ).fetchone()
                if previous is None or tuple(previous) != values:
                    raise ValueError("Ten sam identyfikator ma inna tresc; pomiar niepotwierdzony")
            db.execute("COMMIT")
            return inserted

    def devices(self):
        with self.connection() as db:
            rows = db.execute(
                "SELECT d.*,m.timestamp_ms,m.received_ms,m.temperature_sht_c,m.humidity_rh,m.temperature_ds18_c,m.sht45_error,m.ds18b20_error FROM devices d LEFT JOIN measurements m ON m.id=d.latest_id ORDER BY d.device_id"
            ).fetchall()
        result = []
        for row in rows:
            item = dict(row)
            item["status"] = json.loads(item.pop("status_json"))
            result.append(item)
        return result

    def measurements(self, mac, start, end, limit=200, before=None):
        mac = canonical_mac(mac)
        integer(start, 0, MAX_I64)
        integer(end, start, MAX_I64)
        integer(limit, 1, 1000)
        if before is not None:
            integer(before, 1, MAX_I64)
        with self.connection() as db:
            rows = db.execute(
                "SELECT * FROM measurements WHERE device_id=? AND effective_ms>=? AND effective_ms<=?"
                + (" AND id<?" if before else "")
                + " ORDER BY id DESC LIMIT ?",
                (mac, start, end) + ((before,) if before else ()) + (limit + 1,),
            ).fetchall()
        page = [
            {
                **dict(row),
                "sht45_valid": bool(row["sht45_valid"]),
                "ds18b20_valid": bool(row["ds18b20_valid"]),
            }
            for row in rows[:limit]
        ]
        return {"items": page, "next_before": page[-1]["id"] if len(rows) > limit else None}

    @contextmanager
    def export_range(self, mac, start, end):
        """A consistent read snapshot; rows are consumed incrementally by the response."""
        mac = canonical_mac(mac)
        integer(start, 0, MAX_I64)
        integer(end, start + 1, MAX_I64)
        with self.connection() as db:
            db.execute("BEGIN")
            cursor = db.execute(
                "SELECT * FROM measurements WHERE device_id=? AND effective_ms>=? AND effective_ms<=? ORDER BY id DESC",
                (mac, start, end),
            )
            yield cursor

    def chart(self, mac, start, end, buckets=600):
        mac = canonical_mac(mac)
        integer(start, 0, MAX_I64)
        integer(end, start + 1, MAX_I64)
        integer(buckets, 1, 1000)
        width = max(1, (end - start + 1 + buckets - 1) // buckets)
        with self.connection() as db:
            rows = db.execute(
                """SELECT CAST((timestamp_ms-?)/? AS INTEGER) AS bucket,MIN(timestamp_ms) AS timestamp_ms,
                AVG(temperature_sht_c) AS temperature_sht_c,AVG(humidity_rh) AS humidity_rh,AVG(temperature_ds18_c) AS temperature_ds18_c,
                MIN(temperature_sht_c) AS min_sht,MAX(temperature_sht_c) AS max_sht,
                MIN(temperature_ds18_c) AS min_ds,MAX(temperature_ds18_c) AS max_ds,
                MIN(humidity_rh) AS min_rh,MAX(humidity_rh) AS max_rh,COUNT(*) AS samples
                FROM measurements WHERE device_id=? AND effective_ms>=? AND effective_ms<=? AND timestamp_ms IS NOT NULL
                GROUP BY bucket ORDER BY bucket""",
                (start, width, mac, start, end),
            ).fetchall()
        return {"items": [dict(row) for row in rows], "bucket_ms": width}

    def name(self, mac, value):
        mac = canonical_mac(mac)
        if (
            not isinstance(value, str)
            or len(value) > 80
            or any(ord(c) < 32 or 127 <= ord(c) <= 159 for c in value)
        ):
            raise ValueError("Nazwa ma do 80 znakow bez znakow sterujacych")
        with self.connection() as db:
            cursor = db.execute("UPDATE devices SET name=? WHERE device_id=?", (value, mac))
            if not cursor.rowcount:
                raise ValueError("Najpierw poczekaj na pomiar urzadzenia")

    def backup(self, output):
        target = Path(output).expanduser().absolute()
        if target.parent.stat().st_mode & 0o077:
            raise ValueError("Kopia wymaga prywatnego katalogu 700")
        fd = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
        os.close(fd)
        try:
            with self.connection() as source:
                destination = sqlite3.connect(target)
                try:
                    source.backup(destination, pages=256)
                finally:
                    destination.close()
        except BaseException:
            target.unlink(missing_ok=True)
            raise
