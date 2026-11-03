#pragma once
#include "esp_event.h"
#include <sys/time.h>
inline constexpr char NETIF_SNTP_EVENT[] = "sntp";
inline constexpr int32_t NETIF_SNTP_TIME_SYNC = 0;
struct esp_netif_sntp_time_sync_t {
    timeval tv;
};
struct esp_sntp_config_t {
    bool wait_for_sync;
    bool start;
    const char *server;
};
#define ESP_NETIF_SNTP_DEFAULT_CONFIG(server)                                                                \
    esp_sntp_config_t { true, true, server }
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *);
esp_err_t esp_netif_sntp_start();
void esp_netif_sntp_deinit();
