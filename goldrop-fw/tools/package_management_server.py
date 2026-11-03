#!/usr/bin/env python3
"""Prepare a server-only archive; never include device or OTA private keys."""

import argparse
from pathlib import Path
import tarfile


def package(output):
    tools = Path(__file__).resolve().parent
    names = (
        "manage_devices.py",
        "management_registry.py",
        "management_protocol.py",
        "provision_wifi.py",
        "requirements-management.txt",
        "device_identity.py",
        "firmware_catalog.py",
        "fleet_management.py",
        "measurement_store.py",
        "telemetry_protocol.py",
        "serve_measurements.py",
        "web/index.html",
        "web/app.js",
        "web/management.js",
        "web/styles.css",
    )
    output = Path(output)
    output.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(output, "w:gz") as archive:
        for name in names:
            archive.add(tools / name, arcname=name, recursive=False)
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(
        description="Paczka procesu serwerowego Goldrop (bez sekretow)"
    )
    parser.add_argument("--output", default="build/goldrop-management.tar.gz")
    args = parser.parse_args()
    print(package(args.output))
