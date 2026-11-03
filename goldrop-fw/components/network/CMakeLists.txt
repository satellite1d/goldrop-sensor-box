idf_component_register(SRCS "wifi_manager.cpp" "tcp_transport.cpp" "backend_ack.cpp" "async_dns.cpp" "system_clock.cpp" INCLUDE_DIRS "include" REQUIRES model telemetry
                       PRIV_REQUIRES esp_wifi esp_netif esp_event esp_hw_support lwip esp_timer)
