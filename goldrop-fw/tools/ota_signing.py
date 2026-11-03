"""ECDSA P-256 signing through OpenSSL; private keys never enter release files."""
import base64
import hashlib
import os
from pathlib import Path
import subprocess


def openssl(arguments, data=None):
    result = subprocess.run(["openssl", *arguments], input=data, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=15)
    if result.returncode:
        # Do not expose process output that could contain sensitive key material.
        raise ValueError("OpenSSL failed; check the key file and OpenSSL installation")
    return result.stdout


def public_key_pem(private_key):
    der = openssl(["pkey", "-in", str(private_key), "-passin", "pass:", "-pubout", "-outform", "DER"])
    p256 = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d03010703420004")
    if len(der) != 91 or not der.startswith(p256):
        raise ValueError("OTA requires an ECDSA P-256 private key")
    encoded = base64.b64encode(der)
    lines = b"\n".join(encoded[offset:offset + 64] for offset in range(0, len(encoded), 64))
    return b"-----BEGIN PUBLIC KEY-----\n" + lines + b"\n-----END PUBLIC KEY-----\n"


def initialize_keys(private_key, public_key):
    private_key, public_key = Path(private_key), Path(public_key)
    if public_key.exists() and not private_key.exists():
        raise ValueError("Public key exists but private key is missing; restore the original private key backup")
    private_key.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    if not private_key.exists():
        descriptor = os.open(private_key, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        try:
            with os.fdopen(descriptor, "wb") as output:
                result = subprocess.run(["openssl", "genpkey", "-algorithm", "EC", "-pkeyopt",
                                         "ec_paramgen_curve:P-256"], stdin=subprocess.DEVNULL,
                                        stdout=output, stderr=subprocess.PIPE, timeout=15)
            if result.returncode:
                raise ValueError("OpenSSL could not generate an OTA key")
        except BaseException:
            private_key.unlink(missing_ok=True)
            raise
    pem = public_key_pem(private_key)
    public_key.parent.mkdir(parents=True, exist_ok=True)
    if public_key.exists():
        if public_key.read_bytes() != pem:
            raise ValueError("Existing public key belongs to another private key; no files replaced")
    else:
        with public_key.open("xb") as output:
            output.write(pem)
    return hashlib.sha256(pem).hexdigest()


def sign_payload(payload, private_key):
    signature = openssl(["dgst", "-sha256", "-sign", str(private_key), "-passin", "pass:"], payload)
    if not 8 <= len(signature) <= 72:
        raise ValueError("Expected an ECDSA P-256 DER signature")
    return signature
