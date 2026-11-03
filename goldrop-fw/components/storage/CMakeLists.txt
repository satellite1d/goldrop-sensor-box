idf_component_register(SRCS "wifi_config_codec.cpp" "nvs_wifi_config_store.cpp"
                       "server_config_codec.cpp" "nvs_server_config_store.cpp"
                       "ota_config_codec.cpp" "nvs_ota_config_store.cpp"
                       "flash_journal.cpp" "partition_flash_io.cpp" "nvs_sampling_config_store.cpp"
                       "managed_config_codec.cpp" "nvs_managed_config_store.cpp"
                       INCLUDE_DIRS "include" REQUIRES model telemetry esp_partition PRIV_REQUIRES nvs_flash freertos)
