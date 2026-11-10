# goldrop-sensor-box
Temperature and humidity sensor through wifi with 230VAC power input.

The device runs on ESP32-C3-WROOM chip utilizing the built in wifi antenna, and can be powered with usb-c or up to 230V mains.
Main purpose of the device is to collect temperature and humidity data and sending it to a server through wifi in real time.
Firmware has been vibe coded.


## Firmware
Current firmware 0.6.1 for ESP32-C3 using ESP-IDF 6.1 and PlatformIO.
more info in [`goldrop-fw/`](goldrop-fw/) 

## Schematic

Pliki schematu, PCB i bibliotek: [KiCad/](KiCad/).
<img width="3508" height="2480" alt="image" src="https://github.com/user-attachments/assets/ae6e2b10-bccb-43a4-8a64-8e0a54231b27" />

## PCB design
TODO: 
  - add a picture with all components
  - varistor might not be necessary
  - maybe change the LDO to a bigger one for better longevity 
<img width="1165" height="645" alt="image" src="https://github.com/user-attachments/assets/a770c792-8ab8-4e41-96d3-5d3df9132112" />

## Case design
TODO: add case photo and files







