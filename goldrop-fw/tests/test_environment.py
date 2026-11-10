"""Only synthetic local services; never connect to a real SSH host or device."""

import contextlib
import io
from pathlib import Path
import shlex
import socket
import sqlite3
import subprocess
import sys
import tarfile
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import goldrop_env as env
from package_management_server import package


def free_ports(count):
    with contextlib.ExitStack() as stack:
        sockets = [stack.enter_context(socket.socket()) for _ in range(count)]
        for item in sockets:
            item.bind(("127.0.0.1", 0))
        return [item.getsockname()[1] for item in sockets]


class ServerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="goldrop env ")
        self.root = Path(self.tmp.name)
        self.install = self.root / "install"
        self.install.mkdir()
        with tarfile.open(package(self.root / "server.tar.gz")) as archive:
            archive.extractall(self.install, filter="data")
        tcp, web, management = free_ports(3)
        self.args = SimpleNamespace(
            action="start",
            private_dir=str(self.root / "private"),
            ota_dir=str(self.root / "ota"),
            log_dir=str(self.root / "logs"),
            python=sys.executable,
            tcp_port=tcp,
            web_port=web,
            management_port=management,
        )
        # The installed scripts, not this repository's real services, are controlled.
        self.module_path = patch.object(env, "__file__", str(self.install / "goldrop_env.py"))
        self.module_path.start()
        self.services = env.server_services(self.args)
        self.children = []
        real_popen = subprocess.Popen

        def popen(*args, **kwargs):
            child = real_popen(*args, **kwargs)
            self.children.append(child)
            return child

        self.popen_patch = patch.object(env.subprocess, "Popen", side_effect=popen)
        self.popen_patch.start()

    def tearDown(self):
        for service in reversed(self.services):
            service.stop()
        for child in self.children:
            child.wait(timeout=3)
        self.popen_patch.stop()
        self.module_path.stop()
        self.tmp.cleanup()

    def run_action(self, action):
        self.args.action = action
        with contextlib.redirect_stdout(io.StringIO()):
            return env.server(self.args)

    def pids(self):
        return [p.pid for p in env.processes() if any(s.matches(p) for s in self.services)]

    def test_start_status_repeated_start_and_stop_preserve_data(self):
        self.assertEqual(self.run_action("start"), 0)
        before = self.pids()
        self.assertEqual(len(before), 2)
        self.assertEqual(self.run_action("status"), 0)
        self.assertEqual(self.run_action("start"), 0)
        self.assertEqual(self.pids(), before)
        registry = Path(self.args.private_dir) / "devices.sqlite"
        with sqlite3.connect(registry) as db:
            db.execute(
                "INSERT INTO devices(mac,key) VALUES(?,?)", ("02:00:00:00:00:42", bytes(range(32)))
            )
            db.execute(
                "INSERT INTO ota_targets VALUES(?,?)", ("02:00:00:00:00:42", "manifest-0.6.1.json")
            )

        def contents():
            with sqlite3.connect(registry) as db:
                return (
                    db.execute("SELECT * FROM devices").fetchall(),
                    db.execute("SELECT * FROM ota_targets").fetchall(),
                )

        saved = contents()
        self.assertEqual(self.run_action("stop"), 0)
        self.assertEqual(self.run_action("stop"), 0)
        self.assertEqual(self.run_action("status"), 1)
        self.assertEqual(contents(), saved)
        self.assertEqual(self.run_action("start"), 0)
        self.assertEqual(contents(), saved)

    def test_packaged_shell_wrapper_works_outside_its_directory(self):
        argv = [
            str(self.install / "goldrop-server.sh"),
            "start",
            "--python",
            sys.executable,
            "--private-dir",
            self.args.private_dir,
            "--ota-dir",
            self.args.ota_dir,
            "--log-dir",
            self.args.log_dir,
            "--state-dir",
            str(self.root / "state"),
            "--tcp-port",
            str(self.args.tcp_port),
            "--web-port",
            str(self.args.web_port),
            "--management-port",
            str(self.args.management_port),
        ]
        result = subprocess.run(argv, cwd="/tmp", capture_output=True, text=True, timeout=25)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        before = self.pids()
        self.assertEqual(len(before), 2)
        argv[1] = "status"
        result = subprocess.run(argv, cwd="/tmp", capture_output=True, text=True, timeout=15)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        self.assertEqual(self.pids(), before)

    def test_foreign_listener_is_never_stopped_or_replaced(self):
        with socket.socket() as foreign:
            foreign.bind(("127.0.0.1", self.args.management_port))
            foreign.listen()
            with self.assertRaises(env.EnvironmentError):
                self.run_action("start")
            self.assertEqual(self.pids(), [])
            self.run_action("stop")
            self.assertIn(self.args.management_port, env.listeners())
        self.assertFalse(Path(self.args.private_dir).exists())

    def test_receiver_failure_rolls_back_only_newly_started_service(self):
        private = Path(self.args.private_dir)
        private.mkdir(mode=0o700)
        database = private / "measurements.sqlite"
        database.write_bytes(b"not a sqlite database")
        database.chmod(0o600)
        with self.assertRaises(env.EnvironmentError):
            self.run_action("start")
        self.assertEqual(self.pids(), [])
        self.assertEqual(database.read_bytes(), b"not a sqlite database")

    def test_failure_preserves_preexisting_management_service(self):
        private = Path(self.args.private_dir)
        private.mkdir(mode=0o700)
        Path(self.args.ota_dir).mkdir()
        self.services[0].start()
        existing = self.pids()
        database = private / "measurements.sqlite"
        database.write_bytes(b"not a sqlite database")
        database.chmod(0o600)
        with self.assertRaises(env.EnvironmentError):
            self.run_action("start")
        self.assertEqual(self.pids(), existing)

    def test_legacy_manual_receiver_is_detected_and_restart_enables_management(self):
        private = Path(self.args.private_dir)
        private.mkdir(mode=0o700)
        receiver = self.services[1]
        argv = receiver.argv[: receiver.argv.index("--management-db")]
        old = subprocess.Popen(
            argv, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL
        )
        try:
            deadline = time.monotonic() + 8
            while self.args.web_port not in env.listeners() and time.monotonic() < deadline:
                time.sleep(0.05)
            self.assertEqual(receiver.inspect()[0], "conflict")
            with self.assertRaises(env.EnvironmentError):
                self.run_action("start")
            self.assertEqual(self.run_action("restart"), 0)
            old.wait(timeout=3)
            self.assertEqual(receiver.inspect()[0], "running")
        finally:
            if old.poll() is None:
                old.terminate()
            old.wait(timeout=3)

    def test_stop_rechecks_process_identity_before_signal(self):
        child = subprocess.Popen([sys.executable, "-c", "import time; time.sleep(10)"])
        try:
            process = next(p for p in env.processes() if p.pid == child.pid)
            with self.assertRaises(env.EnvironmentError):
                env.terminate(process, lambda p: False)
            self.assertIsNone(child.poll())
        finally:
            child.terminate()
            child.wait(timeout=3)


class ClientTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.args = SimpleNamespace(
            action="start",
            host="user@example.org",
            local_port=18080,
            web_port=8080,
            tcp_port=54321,
            management_port=8000,
            state_dir=self.tmp.name,
            remote_script="goldrop-management/goldrop_env.py",
            tunnel_only=False,
        )

    def tearDown(self):
        self.tmp.cleanup()

    def test_client_starts_server_then_ssh_tunnel_with_local_bind(self):
        calls = []

        def ssh(*args, **kwargs):
            calls.append(args[1:])
            return SimpleNamespace(returncode=1 if "-O" in args else 0)

        with patch.object(env, "ssh", side_effect=ssh), patch.object(
            env, "remote_server", return_value=0
        ) as remote, patch.object(env, "listeners", return_value={}), patch.object(
            env, "panel_health", return_value="API OK"
        ):
            self.assertEqual(env.client(self.args), 0)
        remote.assert_called_once_with(self.args, "start")
        self.assertIn("127.0.0.1:18080:127.0.0.1:8080", calls[-1])
        self.assertIn("ExitOnForwardFailure=yes", calls[-1])

    def test_healthy_but_unrelated_panel_is_not_reused(self):
        self.args.tunnel_only = True
        with patch.object(env, "ssh", return_value=SimpleNamespace(returncode=1)), patch.object(
            env, "listeners", return_value={18080: {"42"}}
        ), patch.object(env, "manual_tunnels", return_value=[]), patch.object(
            env, "port_details", return_value="foreign"
        ), patch.object(
            env, "panel_health"
        ) as health:
            with self.assertRaises(env.EnvironmentError):
                env.client(self.args)
            health.assert_not_called()

    def test_matching_manual_tunnel_is_reused(self):
        process = env.Process(
            42, ["ssh", "-N", "-L", "18080:127.0.0.1:8080", self.args.host], Path("/tmp")
        )
        self.assertTrue(env.tunnel_matches(process, self.args))
        with patch.object(
            env, "ssh", return_value=SimpleNamespace(returncode=1)
        ) as ssh, patch.object(env, "remote_server", return_value=0), patch.object(
            env, "listeners", return_value={18080: {"99"}}
        ), patch.object(
            env, "manual_tunnels", return_value=[process]
        ), patch.object(
            env, "sockets_of", return_value={"99"}
        ), patch.object(
            env, "panel_health", return_value="API OK"
        ):
            self.assertEqual(env.client(self.args), 0)
            self.assertEqual(ssh.call_count, 1)

    def test_status_checks_tunnel_even_when_remote_service_fails(self):
        self.args.action = "status"
        with patch.object(env, "ssh", return_value=SimpleNamespace(returncode=0)), patch.object(
            env, "remote_server", return_value=1
        ), patch.object(env, "panel_health", return_value="API OK") as health:
            self.assertEqual(env.client(self.args), 1)
            health.assert_called_once()

    def test_client_stop_leaves_remote_server_running(self):
        self.args.action = "stop"
        with patch.object(env, "ssh", return_value=SimpleNamespace(returncode=0)), patch.object(
            env, "listeners", return_value={}
        ), patch.object(env, "remote_server") as remote:
            self.assertEqual(env.client(self.args), 0)
            remote.assert_not_called()

    def test_client_restart_restarts_remote_services(self):
        self.args.action = "restart"
        with patch.object(env, "ssh", return_value=SimpleNamespace(returncode=0)), patch.object(
            env, "listeners", return_value={}
        ), patch.object(env, "remote_server", return_value=0) as remote, patch.object(
            env, "panel_health", return_value="API OK"
        ):
            self.assertEqual(env.client(self.args), 0)
            remote.assert_called_once_with(self.args, "restart")

    def test_regular_file_at_control_socket_path_is_preserved(self):
        import hashlib

        identity = hashlib.sha256(
            f"{self.args.host}:{self.args.local_port}:{self.args.web_port}".encode()
        ).hexdigest()[:16]
        control = Path(self.args.state_dir) / f"tunnel-{identity}.sock"
        control.write_text("preserve this file")
        with patch.object(env, "ssh", return_value=SimpleNamespace(returncode=1)), patch.object(
            env, "remote_server", return_value=0
        ), patch.object(env, "listeners", return_value={}):
            with self.assertRaises(env.EnvironmentError):
                env.client(self.args)
        self.assertEqual(control.read_text(), "preserve this file")

    def test_ssh_remote_command_preserves_literal_arguments(self):
        self.args.remote_script = "some dir/script'; touch BAD; '.py"
        with patch.object(env.subprocess, "run", return_value=SimpleNamespace(returncode=0)) as run:
            env.remote_server(self.args, "start")
        argv = run.call_args.args[0]
        self.assertEqual(shlex.split(argv[-1])[1], self.args.remote_script)
        self.assertNotIn("shell", run.call_args.kwargs)

    def test_process_identification_uses_executed_script_only(self):
        script = Path("/tmp/install/manage_devices.py")

        def process(*args):
            return env.Process(123, ["python3", *args], Path("/tmp"))

        self.assertTrue(env.script_process(process("-u", str(script)), script))
        self.assertTrue(
            env.script_process(process("-X", "dev", "install/manage_devices.py"), script)
        )
        self.assertFalse(env.script_process(process("other.py", str(script), "serve"), script))
        self.assertFalse(env.script_process(process("-m", "other", str(script)), script))
        self.assertFalse(env.script_process(process("-mother", str(script)), script))

    def test_lock_rejects_concurrent_operations(self):
        path = Path(self.tmp.name) / "state"
        with env.lock(path, "server.lock"):
            with self.assertRaises(env.EnvironmentError):
                with env.lock(path, "server.lock"):
                    self.fail("second controller acquired the lock")


if __name__ == "__main__":
    unittest.main()
