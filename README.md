# goldrop-sensor-box
Temperature and humidity sensor through wifi with 230VAC power input.


## Firmware i panel pomiarów

Bieżący firmware **0.6.1** dla ESP32-C3, narzędzia serwera i panel WWW znajdują się
w [`goldrop-fw/`](goldrop-fw/). Firmware korzysta z ESP-IDF 6.1 i PlatformIO.

- [Dokumentacja firmware’u i budowania](goldrop-fw/README.md)

Klucze, hasła, bazy SQLite oraz wyniki kompilacji są lokalne i wykluczone z Gita.

## TODO:
bezpiecznik topikowy,
MOV 275 VAC,  - poczytac o tym wiecej
miejsce na filtr EMI.
zanalizować design plytki esp8266

## Links 
https://www.mouser.pl/ProductDetail/Sensirion/SHT45-AD1F-R2?qs=rQFj71Wb1eW5UnD4WT8nwA%3D%3D
https://www.mouser.pl/ProductDetail/MEAN-WELL/IRM-05-5?qs=WkdRfq4wf1OdYoHS8Am9VA%3D%3D
https://www.mouser.pl/ProductDetail/Espressif-Systems/ESP32-C3-WROOM-02-N4?qs=stqOd1AaK7%2FqjTZKEOgfUg%3D%3D
https://www.mouser.pl/ProductDetail/Diodes-Incorporated/AP2112K-3.3TRG1?qs=x6A8l6qLYDDPYHosCdzh%2FA%3D%3D
https://jlcpcb.com/parts/componentSearch?searchTxt=SHT45-AD1F-R2
https://www.eltrox.pl/p/60330-puszka-hermetyczna-100-x-100-x-50-dlawik-ip65-bezhalogenowa-szara-pawbol-s-box-106

tutorial:
https://www.youtube.com/watch?v=UP0-w7tPxF0

espressif schematic and pcb design guide:
https://github.com/espressif/esp-hardware-design-guidelines





