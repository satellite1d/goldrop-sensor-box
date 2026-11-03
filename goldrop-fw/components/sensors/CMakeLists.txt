idf_component_register(SRCS "sensor_service.cpp" "sensor_protocol.cpp" "ds18b20.cpp"
                           "sht45.cpp" "esp_onewire_bus.cpp" "hardware_sensors.cpp"
                       INCLUDE_DIRS "include"
                       REQUIRES model esp_driver_i2c espressif__onewire_bus
                       PRIV_REQUIRES esp_timer)
