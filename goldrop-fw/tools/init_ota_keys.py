#!/usr/bin/env python3
"""Create an OTA signing key and matching public key for the firmware build."""
import argparse
from pathlib import Path
import subprocess
import sys
from ota_signing import initialize_keys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--private-key", type=Path, default=Path("secrets/ota/private.pem"))
    parser.add_argument("--public-key", type=Path, default=Path("config/ota_public_key.pem"))
    args = parser.parse_args()
    try:
        fingerprint = initialize_keys(args.private_key, args.public_key)
    except (OSError, ValueError, subprocess.TimeoutExpired) as error:
        print(str(error), file=sys.stderr)
        return 1
    print(f"Private signing key: {args.private_key} (keep a backup; never upload to OTA server)")
    print(f"Public verification key: {args.public_key}")
    print(f"Public key SHA-256: {fingerprint}")
    print("Build the firmware after this step, then install it through USB once.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
