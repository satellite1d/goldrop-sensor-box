idf_component_register(SRCS "application.cpp" INCLUDE_DIRS "include"
    PRIV_REQUIRES sensors telemetry storage mqtt health network ota provisioning configuration management
                  esp_timer esp_system esp_hw_support esp_app_format nvs_flash)
