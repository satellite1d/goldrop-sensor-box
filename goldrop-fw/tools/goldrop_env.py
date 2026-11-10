#!/usr/bin/env python3
"""Start/check/stop the Linux Goldrop services and the local OpenSSH tunnel."""

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
import fcntl
import hashlib
import http.client
import json
import os
from pathlib import Path
import shlex
import select
import signal
import stat
import subprocess
import sys
import time


class EnvironmentError(RuntimeError):
    pass


@dataclass
class Process:
    pid: int
    argv: list
    cwd: Path


def processes():
    result = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            if entry.stat().st_uid != os.getuid():
                continue
            argv = [os.fsdecode(a) for a in (entry / "cmdline").read_bytes().split(b"\0") if a]
            if argv:
                result.append(Process(int(entry.name), argv, (entry / "cwd").resolve(strict=True)))
        except (OSError, RuntimeError):
            continue  # A process can exit while /proc is being inspected.
    return result


def listeners():
    result = {}
    for table in ("/proc/net/tcp", "/proc/net/tcp6"):
        try:
            lines = Path(table).read_text().splitlines()[1:]
        except FileNotFoundError:
            continue
        for line in lines:
            fields = line.split()
            if fields[3] == "0A":
                port = int(fields[1].rsplit(":", 1)[1], 16)
                result.setdefault(port, set()).add(fields[9])
    return result


def sockets_of(pid):
    result = set()
    try:
        for fd in Path(f"/proc/{pid}/fd").iterdir():
            try:
                target = os.readlink(fd)
                if target.startswith("socket:["):
                    result.add(target[8:-1])
            except OSError:
                continue
    except OSError:
        pass
    return result


def port_details(ports):
    occupied = listeners()
    owners = {}
    for process in processes():
        owned = sockets_of(process.pid)
        for port in ports:
            if occupied.get(port, set()) & owned:
                owners.setdefault(port, []).append(
                    f"PID {process.pid} ({Path(process.argv[0]).name})"
                )
    return "; ".join(
        f"{port}: {', '.join(owners.get(port, ['wlasciciel niedostepny']))}"
        for port in ports
        if occupied.get(port)
    )


def path_at(value, cwd):
    path = Path(value).expanduser()
    return (cwd / path).resolve() if not path.is_absolute() else path.resolve()


def option(argv, name, default=None):
    for index, item in enumerate(argv):
        if item == name:
            return argv[index + 1] if index + 1 < len(argv) else None
        if item.startswith(name + "="):
            return item[len(name) + 1 :]
    return default


def script_process(process, script):
    if not Path(process.argv[0]).name.startswith("python"):
        return False
    args = iter(process.argv[1:])
    for arg in args:
        if arg == "--":
            target = next(args, None)
            return target is not None and path_at(target, process.cwd) == script
        if arg in ("-X", "-W"):
            if next(args, None) is None:
                return False
        elif arg in ("-u", "-B", "-E", "-I", "-s", "-S", "-O", "-OO", "-q"):
            continue
        elif arg.startswith(("-X", "-W")):
            continue
        elif arg.startswith("-"):
            return (
                False  # Includes -c/-m; never match a script name in another program's arguments.
            )
        else:
            return path_at(arg, process.cwd) == script
    return False


def http_json(port, path):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    try:
        connection.request("GET", path, headers={"Host": f"127.0.0.1:{port}"})
        response = connection.getresponse()
        data = response.read(2_000_001)
        if response.status != 200 or len(data) > 2_000_000:
            raise EnvironmentError(f"HTTP {response.status}: {path}")
        return json.loads(data)
    finally:
        connection.close()


def panel_health(port):
    devices = http_json(port, "/api/devices")
    management = http_json(port, "/api/management")
    if not isinstance(devices, dict) or not isinstance(devices.get("devices"), list):
        raise EnvironmentError("Port panelu nie zwraca API Goldrop")
    if not isinstance(management, dict) or management.get("enabled") is not True:
        raise EnvironmentError("Panel dziala bez zarzadzania; uzyj serwerowego restart")
    return f"API OK, urzadzen: {len(devices['devices'])}, zarzadzanie wlaczone"


def management_health(port):
    connection = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    try:
        connection.request("GET", "/manifest.json")
        response = connection.getresponse()
        response.read(8193)
        if response.status not in (200, 404):
            raise EnvironmentError(f"Usluga OTA: HTTP {response.status}")
        return "HTTP OK" if response.status == 200 else "HTTP OK; brak globalnego manifest.json"
    finally:
        connection.close()


@contextmanager
def lock(directory, name):
    directory.mkdir(parents=True, mode=0o700, exist_ok=True)
    if (
        directory.is_symlink()
        or directory.stat().st_uid != os.getuid()
        or directory.stat().st_mode & 0o077
    ):
        raise EnvironmentError(
            f"Katalog stanu musi nalezec do Ciebie i miec uprawnienia 700: {directory}"
        )
    fd = os.open(directory / name, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError as error:
            raise EnvironmentError(
                "Inne polecenie Goldrop juz dziala; poczekaj na jego zakonczenie"
            ) from error
        yield
    finally:
        os.close(fd)


def terminate(process, matches):
    # pidfd ties the signal to the inspected process, rather than a possibly reused PID.
    try:
        fd = os.pidfd_open(process.pid)
    except ProcessLookupError:
        return
    try:
        current = next((p for p in processes() if p.pid == process.pid), None)
        if not current or current.argv != process.argv or not matches(current):
            raise EnvironmentError(
                f"PID {process.pid} zmienil wlasciciela lub polecenie; nie zatrzymano"
            )
        signal.pidfd_send_signal(fd, signal.SIGTERM)
        readable, _, _ = select.select([fd], [], [], 12)
        if not readable:
            raise EnvironmentError(
                f"PID {process.pid} nie zakonczyl sie po SIGTERM; nie wymuszam SIGKILL"
            )
    finally:
        os.close(fd)


@dataclass
class Service:
    name: str
    script: Path
    argv: list
    ports: tuple
    logfile: Path
    paths: dict
    values: dict
    health: object

    def matches(self, process, complete=True):
        if not script_process(process, self.script) or "serve" not in process.argv:
            return False
        for key, value in self.paths.items():
            actual = option(process.argv, key)
            if actual is None:
                if (
                    not complete
                    and key in ("--management-db", "--ota-dir")
                    and self.name == "pomiary"
                ):
                    continue
                return False
            if path_at(actual, process.cwd) != value:
                return False
        return all(
            option(process.argv, key, str(default)) == str(value)
            for key, (value, default) in self.values.items()
        )

    def inspect(self):
        candidates = [p for p in processes() if self.matches(p, complete=False)]
        occupied = listeners()
        busy = [port for port in self.ports if occupied.get(port)]
        if not candidates:
            return "conflict" if busy else "stopped", (
                "porty zajete: " + port_details(busy) if busy else "nie dziala"
            )
        if len(candidates) != 1:
            return "conflict", "wiele procesow: " + ", ".join(str(p.pid) for p in candidates)
        process = candidates[0]
        if not self.matches(process):
            return "conflict", f"PID {process.pid}: inne opcje zarzadzania; uzyj restart"
        owned = sockets_of(process.pid)
        if any(not occupied.get(port) or not occupied[port].issubset(owned) for port in self.ports):
            return (
                "conflict",
                f"PID {process.pid}: brak oczekiwanych nasluchow lub konflikt portow {self.ports}",
            )
        try:
            detail = self.health()
        except (EnvironmentError, OSError, ValueError, http.client.HTTPException) as error:
            return "unhealthy", f"PID {process.pid}: {error}"
        return "running", f"PID {process.pid}; {detail}"

    def start(self):
        self.logfile.parent.mkdir(parents=True, exist_ok=True)
        fd = os.open(self.logfile, os.O_WRONLY | os.O_CREAT | os.O_APPEND | os.O_NOFOLLOW, 0o600)
        try:
            os.fchmod(fd, 0o600)
            child = subprocess.Popen(
                self.argv,
                stdin=subprocess.DEVNULL,
                stdout=fd,
                stderr=fd,
                start_new_session=True,
                cwd=self.script.parent,
            )
        finally:
            os.close(fd)
        try:
            deadline = time.monotonic() + 15
            while child.poll() is None and time.monotonic() < deadline:
                state, detail = self.inspect()
                if state == "running":
                    print(f"{self.name}: {detail}", flush=True)
                    return child
                time.sleep(0.15)
        except (KeyboardInterrupt, OSError):
            self.cleanup_child(child)
            raise
        self.cleanup_child(child)
        raise EnvironmentError(f"{self.name}: start nieudany; sprawdz {self.logfile}")

    def cleanup_child(self, child):
        if child.poll() is None:
            current = next((p for p in processes() if p.pid == child.pid), None)
            if current:
                terminate(current, self.matches)
        child.wait(timeout=2)

    def stop(self):
        matching = [p for p in processes() if self.matches(p, complete=False)]
        for process in matching:
            terminate(process, lambda p: self.matches(p, complete=False))
            print(f"{self.name}: zatrzymano PID {process.pid}", flush=True)
        if not matching:
            print(f"{self.name}: brak pasujacego procesu", flush=True)


def server_services(args):
    directory = Path(__file__).resolve().parent
    private = Path(args.private_dir).expanduser().resolve()
    ota = Path(args.ota_dir).expanduser().resolve()
    if private == ota or ota in private.parents:
        raise EnvironmentError("Prywatne bazy musza znajdowac sie poza katalogiem OTA")
    python = args.python
    if not python:
        candidates = [
            directory / "venv/bin/python",
            directory.parent / ".venv/bin/python",
            directory.parent / "build/management-venv/bin/python",
        ]
        python = next((str(p) for p in candidates if p.is_file()), sys.executable)
    python = os.path.abspath(os.path.expanduser(python))  # Preserve venv symlinks.
    logs = Path(args.log_dir).expanduser().resolve()
    registry = private / "devices.sqlite"
    measurements = private / "measurements.sqlite"
    return [
        Service(
            "zarzadzanie/OTA",
            directory / "manage_devices.py",
            [
                python,
                "-u",
                str(directory / "manage_devices.py"),
                "--db",
                str(registry),
                "serve",
                "--port",
                str(args.management_port),
                "--ota-dir",
                str(ota),
            ],
            (args.management_port,),
            logs / "goldrop-management.log",
            {"--db": registry, "--ota-dir": ota},
            {"--port": (args.management_port, 8000)},
            lambda: management_health(args.management_port),
        ),
        Service(
            "pomiary",
            directory / "serve_measurements.py",
            [
                python,
                "-u",
                str(directory / "serve_measurements.py"),
                "--db",
                str(measurements),
                "serve",
                "--tcp-port",
                str(args.tcp_port),
                "--web-port",
                str(args.web_port),
                "--management-db",
                str(registry),
                "--ota-dir",
                str(ota),
            ],
            (args.tcp_port, args.web_port),
            logs / "goldrop-measurements.log",
            {"--db": measurements, "--management-db": registry, "--ota-dir": ota},
            {"--tcp-port": (args.tcp_port, 54321), "--web-port": (args.web_port, 8080)},
            lambda: panel_health(args.web_port),
        ),
    ]


def server(args):
    services = server_services(args)
    if args.action in ("stop", "restart"):
        for service in reversed(services):
            service.stop()
        if args.action == "stop":
            return 0
    statuses = [service.inspect() for service in services]
    for service, (state, detail) in zip(services, statuses):
        print(f"{service.name}: {state}; {detail}; log: {service.logfile}", flush=True)
    if args.action == "status":
        return 0 if all(state == "running" for state, _ in statuses) else 1
    if any(state not in ("stopped", "running") for state, _ in statuses):
        raise EnvironmentError(
            "Konflikt lub niesprawna usluga. Sprawdz PID/porty; dla wlasnych uslug uzyj restart"
        )
    for service in services:
        if not service.script.is_file():
            raise EnvironmentError(f"Brak pliku: {service.script}")
    python = services[0].argv[0]
    try:
        result = subprocess.run(
            [python, "-c", "import cryptography"], capture_output=True, timeout=10
        )
    except OSError as error:
        raise EnvironmentError(f"Brak interpretera: {python}") from error
    if result.returncode:
        raise EnvironmentError(
            f"Brak cryptography. Zainstaluj requirements-management.txt w venv i podaj --python SCIEZKA/venv/bin/python"
        )
    private = Path(args.private_dir).expanduser()
    private.mkdir(parents=True, mode=0o700, exist_ok=True)
    if private.is_symlink() or private.stat().st_mode & 0o077:
        raise EnvironmentError(f"Katalog baz musi miec uprawnienia 700: {private}")
    Path(args.ota_dir).expanduser().mkdir(parents=True, exist_ok=True)
    started = []
    try:
        for service, (state, _) in zip(services, statuses):
            if state == "stopped":
                child = service.start()
                started.append((service, child.pid))
    except (EnvironmentError, OSError, KeyboardInterrupt):
        for service, pid in reversed(started):
            process = next((p for p in processes() if p.pid == pid), None)
            if process:
                terminate(process, service.matches)
        raise
    print(f"Gotowe. Panel na serwerze: http://127.0.0.1:{args.web_port}/", flush=True)
    return 0


def ssh(args, *options, capture=False):
    return subprocess.run(
        ["ssh", "-o", "ConnectTimeout=15", *options, args.host], capture_output=capture, text=True
    )


def remote_server(args, action):
    command = shlex.join(
        [
            "python3",
            args.remote_script,
            "server",
            action,
            "--tcp-port",
            str(args.tcp_port),
            "--web-port",
            str(args.web_port),
            "--management-port",
            str(args.management_port),
        ]
    )
    # OpenSSH joins arguments into a remote shell command; quote its contents explicitly.
    return subprocess.run(["ssh", "-o", "ConnectTimeout=15", args.host, command]).returncode


def tunnel_matches(process, args):
    if Path(process.argv[0]).name != "ssh" or args.host not in process.argv:
        return False
    forwards = []
    for index, item in enumerate(process.argv):
        if item == "-L" and index + 1 < len(process.argv):
            forwards.append(process.argv[index + 1])
        elif item.startswith("-L") and len(item) > 2:
            forwards.append(item[2:])
    expected = f"{args.local_port}:127.0.0.1:{args.web_port}"
    return any(
        value in (expected, "127.0.0.1:" + expected, "localhost:" + expected) for value in forwards
    )


def manual_tunnels(args):
    return [p for p in processes() if tunnel_matches(p, args)]


def client(args):
    identity = hashlib.sha256(
        f"{args.host}:{args.local_port}:{args.web_port}".encode()
    ).hexdigest()[:16]
    control = Path(args.state_dir).expanduser().absolute() / f"tunnel-{identity}.sock"
    if len(os.fsencode(control)) >= 104:
        raise EnvironmentError("Za dluga sciezka gniazda SSH; podaj krotszy --state-dir")
    managed = ssh(args, "-S", str(control), "-O", "check", capture=True).returncode == 0
    if args.action in ("stop", "restart"):
        if managed:
            result = ssh(args, "-S", str(control), "-O", "exit", capture=True)
            if result.returncode:
                raise EnvironmentError("Nie udalo sie zatrzymac tunelu SSH")
        else:
            for process in manual_tunnels(args):
                terminate(process, lambda p: tunnel_matches(p, args))
                print(f"Tunel SSH: zatrzymano PID {process.pid}", flush=True)
        deadline = time.monotonic() + 3
        while listeners().get(args.local_port) and time.monotonic() < deadline:
            time.sleep(0.1)
        if listeners().get(args.local_port):
            raise EnvironmentError("Port lokalny nadal zajety: " + port_details([args.local_port]))
        print("Tunel SSH: zatrzymany; uslugi serwera wymagaja osobnego polecenia stop", flush=True)
        if args.action == "stop":
            return 0
        managed = False
    remote_code = 0
    if not args.tunnel_only:
        action = (
            "status"
            if args.action == "status"
            else ("restart" if args.action == "restart" else "start")
        )
        remote_code = remote_server(args, action)
        if remote_code and args.action != "status":
            raise EnvironmentError("Serwer zglosil problem; sprawdz komunikaty powyzej")
    if not managed:
        occupied = listeners().get(args.local_port, set())
        if occupied:
            owned = set().union(*(sockets_of(p.pid) for p in manual_tunnels(args)))
            if not occupied.issubset(owned):
                raise EnvironmentError(
                    "Konflikt portu lokalnego: " + port_details([args.local_port])
                )
            try:
                detail = panel_health(args.local_port)
            except (EnvironmentError, OSError, ValueError, http.client.HTTPException) as error:
                raise EnvironmentError(
                    f"Istniejacy tunel nie daje dostepu do panelu: {error}"
                ) from error
            print(
                f"Istniejacy tunel do {args.host}: {detail}\nOtworz http://127.0.0.1:{args.local_port}/",
                flush=True,
            )
            return 1 if remote_code else 0
        if args.action == "status":
            print("Tunel SSH: nie dziala", flush=True)
            return 1
        try:
            info = control.lstat()
        except FileNotFoundError:
            pass
        else:
            if not stat.S_ISSOCK(info.st_mode) or info.st_uid != os.getuid():
                raise EnvironmentError(f"Nieprawidlowy pozostawiony plik gniazda SSH: {control}")
            control.unlink()
        result = ssh(
            args,
            "-fNT",
            "-M",
            "-S",
            str(control),
            "-o",
            "ControlPersist=no",
            "-o",
            "ExitOnForwardFailure=yes",
            "-o",
            "ServerAliveInterval=30",
            "-o",
            "ServerAliveCountMax=3",
            "-L",
            f"127.0.0.1:{args.local_port}:127.0.0.1:{args.web_port}",
        )
        if result.returncode:
            raise EnvironmentError("Nie udalo sie uruchomic tunelu SSH")
    try:
        detail = panel_health(args.local_port)
    except (EnvironmentError, OSError, ValueError, http.client.HTTPException) as error:
        raise EnvironmentError(
            f"Tunel istnieje, ale panel nie odpowiada poprawnie: {error}"
        ) from error
    print(f"Tunel SSH: OK; {detail}\nOtworz http://127.0.0.1:{args.local_port}/", flush=True)
    return 1 if remote_code else 0


def port(value):
    number = int(value)
    if not 1 <= number <= 65535:
        raise argparse.ArgumentTypeError("Port musi wynosic 1–65535")
    return number


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="mode", required=True)
    for mode in ("server", "client"):
        command = sub.add_parser(mode)
        command.add_argument("action", choices=("start", "status", "stop", "restart"))
        command.add_argument("--tcp-port", type=port, default=54321)
        command.add_argument("--web-port", type=port, default=8080)
        command.add_argument("--management-port", type=port, default=8000)
        command.add_argument("--state-dir", default=str(Path.home() / ".local/state/goldrop"))
        if mode == "server":
            command.add_argument("--private-dir", default=str(Path.home() / "goldrop-private"))
            command.add_argument("--ota-dir", default=str(Path.home() / "goldrop-ota"))
            command.add_argument("--log-dir", default=str(Path.home()))
            command.add_argument("--python", help="Python z venv zawierajacego cryptography")
        else:
            command.add_argument(
                "--host", required=True, help="USER@SERVER albo alias z ~/.ssh/config"
            )
            command.add_argument("--local-port", type=port, default=8080)
            command.add_argument("--remote-script", default="goldrop-management/goldrop_env.py")
            command.add_argument(
                "--tunnel-only", action="store_true", help="Pomin polecenia na serwerze"
            )
    args = parser.parse_args(argv)
    if not Path("/proc/net/tcp").is_file() or not hasattr(os, "pidfd_open"):
        parser.error("Skrypty wymagaja Linux i Python 3.10+ z obsluga pidfd")
    if len({args.tcp_port, args.web_port, args.management_port}) != 3:
        parser.error("Porty trzech uslug musza byc rozne")
    if args.mode == "client" and (args.host.startswith("-") or any(c.isspace() for c in args.host)):
        parser.error("Podaj pojedynczy host SSH, bez opcji i spacji")
    lock_name = (
        "server.lock"
        if args.mode == "server"
        else "client-" + hashlib.sha256(args.host.encode()).hexdigest()[:16] + ".lock"
    )
    with lock(Path(args.state_dir).expanduser(), lock_name):
        return server(args) if args.mode == "server" else client(args)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (EnvironmentError, OSError, subprocess.TimeoutExpired) as error:
        print(f"Blad: {error}", file=sys.stderr)
        raise SystemExit(1)
    except KeyboardInterrupt:
        print("Przerwano.", file=sys.stderr)
        raise SystemExit(130)
