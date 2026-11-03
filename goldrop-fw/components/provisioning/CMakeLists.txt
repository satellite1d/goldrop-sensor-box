idf_component_register(SRCS "wifi_provisioner.cpp" "usb_wifi_console.cpp"
                       INCLUDE_DIRS "include" REQUIRES storage network ota management
                       PRIV_REQUIRES esp_driver_usb_serial_jtag)
