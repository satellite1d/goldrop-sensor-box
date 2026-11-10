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

Zastąp `PORT` portem swojej płytki. Konfiguracja `goldrop` dotyczy własnej PCB z natywnym USB.

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

Skrypty wymagają Linux, Python 3.10+ oraz OpenSSH na komputerze użytkownika.
Paczka serwera zawiera oba procesy, panel i skrypty sterujące:

```sh
python3 tools/package_management_server.py
scp build/goldrop-management.tar.gz USER@SERVER:~/
```

Na serwerze rozpakuj paczkę. Przy pierwszej instalacji przygotuj też venv:

```sh
mkdir -p ~/goldrop-management
tar -xzf ~/goldrop-management.tar.gz -C ~/goldrop-management
python3 -m venv ~/goldrop-management/venv
~/goldrop-management/venv/bin/python -m pip install \
  -r ~/goldrop-management/requirements-management.txt
```

Jeśli tworzenie venv zgłasza brak `ensurepip`, na Ubuntu zainstaluj systemowy pakiet
`python3-venv` i ponów tworzenie środowiska. Istniejące bazy oraz klucze zachowaj.
Skrypty startowe sprawdzają interpreter i zależności; nie instalują pakietów systemowych.

### Jedno polecenie z komputera

W katalogu `goldrop-fw`:

```sh
./tools/goldrop-client.sh start --host USER@SERVER
```

Polecenie uruchamia przez SSH brakujące usługi serwera, sprawdza ich porty/API,
a następnie zestawia tunel i wypisuje adres **http://127.0.0.1:8080/**.
Uwierzytelnienie SSH korzysta ze zwykłych kluczy, agenta lub interaktywnego hasła;
żadne hasło SSH nie jest zapisywane przez skrypt.

```sh
./tools/goldrop-client.sh status --host USER@SERVER
./tools/goldrop-client.sh restart --host USER@SERVER
./tools/goldrop-client.sh stop --host USER@SERVER
```

`status` sprawdza serwer i lokalny tunel. `restart` restartuje usługi serwera
oraz tunel. `stop` zamyka wyłącznie lokalny tunel — odbiór pomiarów na serwerze trwa dalej.
Opcja `--tunnel-only` pomija polecenia na serwerze; `--local-port 8081` wybiera
inny lokalny port przeglądarki. Przy innej instalacji serwera podaj
`--remote-script SCIEZKA/goldrop_env.py` (ścieżka względna do katalogu domowego SSH
lub bezwzględna; bez skrótu `~/`).

### Polecenia bezpośrednio na serwerze

```sh
~/goldrop-management/goldrop-server.sh start
~/goldrop-management/goldrop-server.sh status
~/goldrop-management/goldrop-server.sh restart
~/goldrop-management/goldrop-server.sh stop
```

| Proces | Porty | Kontrola |
| --- | --- | --- |
| Zarządzanie i OTA | 8000 | Proces, konfiguracja, właściciel portu i odpowiedź HTTP |
| Odbiornik pomiarów i panel | 54321, 8080 | Proces, konfiguracja, oba nasłuchy i API SQLite/zarządzania |
| Tunel SSH na komputerze | lokalny 8080 | Sterowanie OpenSSH, zgodność celu i API panelu |

Ponowny `start` zachowuje działające, zgodne procesy, także uruchomione wcześniej
ręcznie przez `nohup`. Starszy odbiornik bez flag zarządzania wymaga `restart`.
Konflikt z innym programem jest zgłaszany z portem i dostępnym PID-em; skrypt
nie zatrzymuje obcych procesów. Nieudany start wycofuje tylko nowo uruchomione usługi.

Domyślne dane to `~/goldrop-private/devices.sqlite`,
`~/goldrop-private/measurements.sqlite` oraz `~/goldrop-ota/`.
Katalog baz jest prywatny (`700`); pierwsze uruchomienie może utworzyć pusty
rejestr zarządzania, ale parowanie/import kluczy urządzeń pozostaje osobnym krokiem.
Brak globalnego `manifest.json` jest informacją, a nie błędem startu usługi.
Logi trafiają do `~/goldrop-management.log` i `~/goldrop-measurements.log`.
Stan blokad i gniazda tunelu jest w `~/.local/state/goldrop/`.

Serwerowy `--help` opisuje opcje `--private-dir`, `--ota-dir`, `--log-dir`,
`--python` oraz porty. Przy niestandardowych portach podaj te same wartości
w skrypcie lokalnym i serwerowym. Inne katalogi danych ustawiaj bezpośrednio
na serwerze, a klienta uruchamiaj z `--tunnel-only`.

Usługi działają w tle po zamknięciu SSH. Po restarcie serwera ponów `start`;
skrypty nie instalują usług systemd ani ciągłego nadzoru po awarii.
Kody wyjścia: `0` — sukces/zdrowe środowisko, `1` — brak usługi, konflikt lub błąd,
`2` — niepoprawne argumenty. Sam odbiornik bez zarządzania nadal można uruchomić
przez `python3 tools/serve_measurements.py --help`.

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
