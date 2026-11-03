idf_component_register(SRCS "management_protocol.cpp" "management_crypto.cpp" "management_service.cpp"
    INCLUDE_DIRS "include" REQUIRES configuration model
    PRIV_REQUIRES esp_http_client mbedtls esp_timer esp_hw_support)
