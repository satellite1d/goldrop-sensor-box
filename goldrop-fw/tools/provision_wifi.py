#!/usr/bin/env python3
"""Configure Goldrop through USB; credentials are never arguments or files."""
import argparse
import getpass
import ipaddress
from pathlib import Path
import re
import sys
import subprocess
import time


def normalize_server_host(value):
    """Match firmware validation; keep DNS resolution on the device."""
    value = value.strip()
    if not value or len(value) > 253 or not value.isascii():
        raise ValueError("invalid server host")
    if all(c in "0123456789." for c in value):
        address = ipaddress.IPv4Address(value)
        if int(address) >> 24 == 0 or int(address) >> 24 >= 224:
            raise ValueError("unsupported server address")
        return str(address)
    for label in value.split("."):
        if not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?", label):
            raise ValueError("invalid DNS label")
    return value.lower()


def main():
    parser = argparse.ArgumentParser(description="Konfiguracja Wi-Fi i serwera TCP Goldrop przez USB")
    parser.add_argument("--port", required=True, help="np. /dev/ttyACM3; zamknij device monitor")
    parser.add_argument("--status", action="store_true", help="Tylko odczytaj stan Wi-Fi i serwera TCP")
    parser.add_argument("--upload", choices=("pio", "idf"), help="Zbuduj i wgraj firmware, potem zapytaj o Wi-Fi i serwer")
    parser.add_argument("--ota", choices=("configure", "status", "check", "retry", "disable", "forget"), help="Konfiguracja lub sterowanie OTA bez zmiany Wi-Fi")
    args = parser.parse_args()
    if args.status and (args.upload or args.ota):
        parser.error("--status nie mozna laczyc z --upload ani --ota")
    try:
        import serial
    except ImportError:
        print("Brak pyserial. Użyj Pythona ze środowiska PlatformIO lub zainstaluj pyserial.", file=sys.stderr)
        return 1

    ssid = password = None
    ansi = re.compile(r"\x1b\[[0-9;]*[A-Za-z]")
    port = serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=3)
    port.dtr = False
    port.rts = False
    port.port = args.port
    try:
        if args.upload:
            upload = (["pio", "run", "-e", "goldrop", "-t", "upload", "--upload-port", args.port]
                      if args.upload == "pio" else ["idf.py", "-B", "build/native", "-p", args.port, "flash"])
            print("Budowanie i wgrywanie firmware'u; po zakonczeniu rozpocznie sie konfiguracja.", flush=True)
            result = subprocess.run(upload, cwd=Path(__file__).resolve().parents[1])
            if result.returncode:
                raise RuntimeError("Wgrywanie nie powiodlo sie; konfiguracja nie zostala uruchomiona.")
            # Allow native USB to re-enumerate after reset; credentials are not sent yet.
            deadline = time.monotonic() + 15
            while True:
                try:
                    port.open()
                    break
                except serial.SerialException:
                    if time.monotonic() >= deadline:
                        raise
                    time.sleep(0.25)
        else:
            port.open()

        def command(line, expected, repeat=False):
            data = (line + "\n").encode("utf-8")
            def write_line():
                # Keep input bursts below the native USB FIFO capacity.
                for offset in range(0, len(data), 16):
                    port.write(data[offset:offset + 16])
                    time.sleep(0.06)
            write_line()
            deadline = time.monotonic() + 10
            next_repeat = time.monotonic() + 1
            while time.monotonic() < deadline:
                if repeat and time.monotonic() >= next_repeat:
                    write_line()  # Read-only status query, safe if the first query preceded boot.
                    next_repeat = time.monotonic() + 1
                text = ansi.sub("", port.read_until(b"\n", 4096).decode("utf-8", errors="replace"))
                index = text.find("usb_config: CFG ")
                if index < 0:
                    continue
                reply = text[index + len("usb_config: "):].strip()
                if reply.startswith("CFG ERROR"):
                    raise RuntimeError(reply)
                if reply.startswith(expected):
                    return reply
            raise RuntimeError("Brak odpowiedzi USB. Sprawdź port, firmware i zamknij device monitor.")

        print(command("wifi status", "CFG STATUS", repeat=True))
        print(command("server status", "CFG SERVER", repeat=True))
        if args.ota:
            ota_status = command("ota status", "CFG OTA", repeat=True)
            print(ota_status)
            if args.ota == "configure":
                from urllib.parse import urlsplit
                url = input("URL manifestu OTA (http://... lub https://.../manifest.json): ").strip()
                channel = input("Kanal [stable]: ").strip() or "stable"
                interval = input("Sprawdzanie co ile sekund [900]: ").strip() or "900"
                automatic = input("Automatycznie instalowac aktualizacje? [t/N]: ").strip().lower() in ("t", "tak", "y", "yes")
                try:
                    parsed = urlsplit(url)
                    if len(url) >= 384 or not url.isascii() or not url.startswith(("https://", "http://")) or not parsed.hostname or parsed.netloc.endswith(":") or parsed.username or parsed.password or parsed.fragment or any(ord(c)<33 or c in "\\@" for c in url):
                        raise ValueError("URL")
                    normalize_server_host(parsed.hostname)
                    if parsed.port is not None and not 1 <= parsed.port <= 65535:
                        raise ValueError("port")
                    if not re.fullmatch(r"[a-z0-9_-]{1,16}", channel) or not re.fullmatch(r"[0-9]{1,5}", interval) or not 60 <= int(interval) <= 86400:
                        raise ValueError("policy")
                except ValueError:
                    print("Nieprawidlowy URL HTTP(S), kanal lub interwal (60–86400 s).", file=sys.stderr)
                    return 1
                if parsed.scheme == "http" and "signed_http=true" not in ota_status.split():
                    print("HTTP wymaga firmware z kluczem publicznym. Uruchom tools/init_ota_keys.py, zbuduj i wgraj przez USB.", file=sys.stderr)
                    return 1
                command("ota url " + url, "CFG OK OTA FIELD")
                command("ota channel " + channel, "CFG OK OTA FIELD")
                command("ota interval " + str(int(interval)), "CFG OK OTA FIELD")
                command("ota enable " + ("1" if automatic else "0"), "CFG OK OTA FIELD")
                print(command("ota save", "CFG OK OTA SAVE"))
            elif args.ota != "status":
                print(command("ota " + args.ota, "CFG OK OTA " + args.ota.upper()))
                if args.ota == "retry":
                    print(command("ota check", "CFG OK OTA CHECK"))
            print(command("ota status", "CFG OTA"))
            return 0
        if args.status:
            return 0
        # Verify the firmware responds before asking the user for credentials.
        ssid = input("Nazwa sieci Wi-Fi (SSID, 2,4 GHz): ")
        password = getpass.getpass("Hasło Wi-Fi (nie będzie wyświetlone): ")
        if not 1 <= len(ssid.encode("utf-8")) <= 32 or any(ord(c) < 32 or ord(c) == 127 for c in ssid):
            print("SSID musi mieć 1–32 bajty UTF-8, bez znaków sterujących.", file=sys.stderr)
            return 1
        if not 8 <= len(password) <= 63 or any(not 32 <= ord(c) <= 126 for c in password):
            print("Hasło musi mieć 8–63 drukowalne znaki ASCII.", file=sys.stderr)
            return 1
        address_text = input("Nazwa DNS lub IPv4 serwera (np. r.limanowa.org): ").strip()
        port_text = input("Port nasluchu serwera (1–65535): ").strip()
        try:
            address = normalize_server_host(address_text)
            if not re.fullmatch(r"[0-9]{1,5}", port_text) or not 1 <= int(port_text) <= 65535:
                raise ValueError("invalid port")
        except ValueError:
            print("Podaj nazwe DNS lub adres IPv4 serwera (bez protokolu i portu) oraz port 1–65535.", file=sys.stderr)
            return 1
        command("server address " + str(address), "CFG OK SERVER ADDRESS")
        command("server port " + str(int(port_text)), "CFG OK SERVER PORT")
        print(command("server save", "CFG OK SERVER SAVE"))
        command("wifi ssid " + ssid, "CFG OK SSID")
        command("wifi password " + password, "CFG OK PASSWORD")
        print(command("wifi save", "CFG OK SAVE"))
        ssid = password = None
        print("Wi-Fi i serwer TCP zapisane w NVS. Oczekiwanie na polaczenie...")
        deadline = time.monotonic() + 45
        last_status = None
        while time.monotonic() < deadline:
            wifi_status = command("wifi status", "CFG STATUS")
            server_status = command("server status", "CFG SERVER")
            status = wifi_status + "\n" + server_status
            if status != last_status:
                print(status)
                last_status = status
            if "state=online " in wifi_status and "state=connected " in server_status:
                return 0
            time.sleep(1)
        print("Dane są zapisane. Wi-Fi lub TCP jeszcze nie dziala; urzadzenie nadal ponawia proby.")
        return 2
    except (serial.SerialException, OSError, RuntimeError) as error:
        print(str(error), file=sys.stderr)
        return 1
    finally:
        ssid = password = None
        port.close()


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (KeyboardInterrupt, EOFError):
        print("\nPrzerwano konfigurację.", file=sys.stderr)
        sys.exit(1)
