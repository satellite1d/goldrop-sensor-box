import os
from pathlib import Path
import subprocess
import sys
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tools"))
from management_protocol import seal, open_envelope, key_id
from cryptography.exceptions import InvalidTag

EXE = sys.argv.pop()
KEY = bytes(range(32))
MAC = "90:da:72:9b:cd:4c"


class Interop(unittest.TestCase):
    def run_cpp(self, mode, body):
        return subprocess.run([EXE, mode], input=body, capture_output=True)

    def test_real_psa_python_both_directions(self):
        for direction, size in ((b"C", 136), (b"S", 512)):
            plain = os.urandom(size)
            cpp = self.run_cpp("encrypt", direction + plain)
            self.assertEqual(cpp.returncode, 0)
            self.assertEqual(open_envelope(KEY, MAC, direction, cpp.stdout, size), plain)
            python = seal(KEY, MAC, direction, plain)
            result = self.run_cpp("decrypt", direction + python)
            self.assertEqual(result.returncode, 0)
            self.assertEqual(result.stdout, plain)
            corrupted = bytearray(python)
            corrupted[-1] ^= 1
            self.assertEqual(self.run_cpp("decrypt", direction + corrupted).returncode, 3)
            other = b"S" if direction == b"C" else b"C"
            self.assertEqual(self.run_cpp("decrypt", other + python).returncode, 3)

    def test_fingerprint(self):
        result = self.run_cpp("fingerprint", b"")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout.decode(), key_id(KEY))


unittest.main()
