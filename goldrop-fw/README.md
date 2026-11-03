# Goldrop firmware

Firmware C++17 dla ESP32-C3-WROOM-02-N4 (4 MB flash) oraz narzędzia serwera
zbierającego pomiary. Bieżąca wersja: **0.6.1**. Projekt używa ESP-IDF 6.1
bez Arduino; konfiguracja PlatformIO jest przypięta do `espressif32` 7.1.3.

## Funkcje

- SHT45: temperatura i wilgotność; DS18B20: temperatura zewnętrzna.
- Jawne stany odłączenia czujników, błędów komunikacji i CRC.
- Wi-Fi 2,4 GHz, konfiguracja przez USB i automatyczne ponawianie połączeń.
- Pomiary domyślnie co 10 s; interwał 10–3600 s zapisywany w NVS.
- Dziennik flash na ponad 24 h przy interwale 10 s.
- Czas UTC synchronizowany przez SNTP i identyfikacja urządzeń przez MAC.
- Telemetria TCP/JSON z potwierdzeniem po zapisie do SQLite i deduplikacją.
- OTA A/B z kontrolą sprzętu, SHA-256, podpisanym manifestem i rollbackiem.
- Szyfrowany kanał zdalnej konfiguracji Wi-Fi i serwera, próba ustawień i cofnięcie.
- Panel WWW: wykresy, historia, CSV, nazwy urządzeń, grupowe OTA, Wi-Fi i interwał.

Po każdym starcie pierwsze dwa pomiary dzieli 10 s na potrzeby testu startowego.
Podczas próby konfiguracji odstęp wynosi najwyżej 10 s; następnie wraca ustawienie NVS.
Pierwszy start po OTA usuwa niewysłany bufor pomiarów. Zwykły restart zachowuje bufor.

## Sprzęt

| Interfejs | GPIO |
| --- | --- |
| SHT45 SDA / SCL | 4 / 5 |
| DS18B20 | 10 |
| Natywne USB D− / D+ | 18 / 19 |

Rewizja sprzętu w obrazach OTA: `goldrop-c3-v1.0`.

## Budowanie

Polecenia wykonuj w katalogu `goldrop-fw`. Pierwsza kompilacja pobiera zależności
ESP-IDF Component Manager; plik `dependencies.lock` utrwala ich wersje.

### PlatformIO

```sh
pio run -e goldrop
pio device list
pio run -e goldrop -t upload --upload-port PORT
pio device monitor --port PORT --baud 115200
```

Zastąp `PORT` portem swojej płytki. Konfiguracja `goldrop` dotyczy własnej PCB
z natywnym USB, nie wcześniejszego projektu w `simulation_test/`.

### ESP-IDF

Aktywuj zainstalowane ESP-IDF **6.1** w swoim terminalu:

```sh
idf.py -B build/idf build
idf.py -B build/idf -p PORT flash monitor
```

Cel `esp32c3`, partycje i ustawienia początkowe są zdefiniowane w projekcie.
Wyniki kompilacji trafiają do `.pio/build/goldrop/` lub `build/idf/`.

## Konfiguracja USB

Przygotuj środowisko narzędzi Python 3.10+:

```sh
python3 -m venv .venv
.venv/bin/python -m pip install -r tools/requirements-management.txt
.venv/bin/python tools/provision_wifi.py --port PORT
```

Zamknij monitor portu przed uruchomieniem konfiguratora. Narzędzie pyta o SSID,
hasło oraz nazwę DNS/IPv4 i port odbiornika. Ustawienia są zapisywane w NVS.
Rejestrację kanału zarządzania obsługuje `tools/provision_management.py --help`.

## Serwer i panel

```sh
python3 tools/package_management_server.py
```

Paczka `build/goldrop-management.tar.gz` zawiera serwer, moduły i pliki strony;
nie zawiera baz ani kluczy. Serwer wymaga Python 3.10+. Sam odbiornik pomiarów
korzysta z biblioteki standardowej; zarządzanie wymaga `requirements-management.txt`.

```sh
python3 tools/serve_measurements.py \
  --db /path/to/private/measurements.sqlite serve \
  --tcp-port 54321 --web-port 8080
```

Katalog baz musi być prywatny (uprawnienia `700`). Panel nasłuchuje na
`127.0.0.1:8080`; dostęp z innego komputera zapewnia tunel SSH:

```sh
ssh -N -L 127.0.0.1:8080:127.0.0.1:8080 USER@SERVER
```

Otwórz `http://127.0.0.1:8080/`. Aby włączyć zakładkę Zarządzanie, uruchom odbiornik
Pythonem z venv i dodaj `--management-db /path/to/private/devices.sqlite`
oraz `--ota-dir /path/to/ota`. Osobny proces obsługuje wymiany z urządzeniami i OTA:

```sh
.venv/bin/python tools/manage_devices.py \
  --db /path/to/private/devices.sqlite serve \
  --port 8000 --ota-dir /path/to/ota
```

Rejestr musi zawierać klucze sparowanych urządzeń. Obie usługi używają tego samego
pliku `devices.sqlite`; baza `measurements.sqlite` ma odrębny format.

## Wydania OTA

Prywatne klucze, lokalny klucz publiczny oraz obrazy nie są przechowywane w repozytorium.
Dla nowej instalacji klucze generuje `python3 tools/init_ota_keys.py`; następnie
zbuduj firmware i zainstaluj go przez USB. Aktualizując działające urządzenia,
używaj dotychczasowego klucza podpisującego i odpowiadającego mu klucza publicznego.

Zwiększ `version.txt`, zbuduj obraz i przygotuj wydanie:

```sh
python3 tools/package_ota.py \
  --base-url http://SERVER:8000 \
  --sign-key secrets/ota/private.pem --output build/ota-http
```

Na serwer kopiuj najpierw binarkę, potem manifest. Do wyboru wydania dla konkretnych
płytek opublikuj manifest jako `manifest-WERSJA.json`. Globalny `manifest.json`
stanowi domyślną ofertę aktualizacji dla urządzeń bez indywidualnego przypisania.
Klucz prywatny pozostaje na komputerze wydawcy. Weryfikację podpisu wykonuje urządzenie.

## Testy

Po pierwszej kompilacji firmware'u dostępne jest przypięte cJSON używane w testach.
Wymagane są CMake, kompilator C/C++17, Python i OpenSSL:

```sh
.venv/bin/python -m pip install --target build/management-deps \
  -r tools/requirements-management.txt
cmake -S tests -B build/host
cmake --build build/host
ctest --test-dir build/host --output-on-failure
```

Testy obejmują czujniki, bufory, konfigurację, czas, DNS, TCP/ACK, OTA oraz backend.
Testy przeglądarki w `tests/test_measurement_browser.py` i `tests/test_fleet_browser.py`
wymagają Playwright i Chromium. Testy hostowe nie zastępują prób na fizycznym sprzęcie.

## Ograniczenia prototypu

Telemetria TCP nie jest jeszcze szyfrowana ani uwierzytelniana. Panel przeznaczony
jest do dostępu przez tunel SSH i nie ma osobnego logowania. HTTP OTA wymaga
podpisanego manifestu; HTTPS korzysta z weryfikacji certyfikatów.
Secure Boot, MQTT/TLS i provisioning BLE nie są zaimplementowane.
