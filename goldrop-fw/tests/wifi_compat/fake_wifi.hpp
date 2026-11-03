#pragma once
#include <cstdint>
#include <cstddef>
using esp_err_t = int;
inline constexpr int ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_NO_MEM = 1, ESP_ERR_INVALID_STATE = 2,
                     ESP_ERR_WIFI_NOT_STARTED = 3;
inline const char *esp_err_to_name(int value) { return value == ESP_OK ? "ESP_OK" : "test_error"; }
using esp_event_base_t = const char *;
using esp_event_handler_instance_t = void *;
using esp_event_handler_t = void (*)(void *, esp_event_base_t, int32_t, void *);
inline constexpr char WIFI_EVENT[] = "wifi", IP_EVENT[] = "ip";
inline constexpr int ESP_EVENT_ANY_ID = -1, WIFI_EVENT_STA_START = 1, WIFI_EVENT_STA_STOP = 2,
                     WIFI_EVENT_STA_CONNECTED = 3, WIFI_EVENT_STA_DISCONNECTED = 4, IP_EVENT_STA_GOT_IP = 5,
                     IP_EVENT_STA_LOST_IP = 6;
struct esp_ip4_addr_t {
    uint32_t addr{};
};
struct esp_netif_t {};
struct esp_netif_config_t {};
struct ip_event_got_ip_t {
    esp_netif_t *esp_netif{};
    struct {
        esp_ip4_addr_t ip;
    } ip_info;
};
struct wifi_event_sta_disconnected_t {
    uint16_t reason{};
};
struct wifi_ap_record_t {
    int8_t rssi{};
};
struct wifi_init_config_t {
    bool nvs_enable{true};
};
struct wifi_config_t {
    struct {
        char ssid[32]{}, password[64]{};
        struct {
            int authmode{};
        } threshold;
        struct {
            bool capable{}, required{};
        } pmf_cfg;
        int sae_pwe_h2e{}, failure_retry_cnt{};
    } sta;
};
inline constexpr int WIFI_STORAGE_RAM = 0, WIFI_MODE_STA = 0, WIFI_AUTH_WPA2_PSK = 0, WPA3_SAE_PWE_BOTH = 0,
                     WIFI_IF_STA = 0;
#define WIFI_INIT_CONFIG_DEFAULT()                                                                           \
    wifi_init_config_t {}
#define ESP_NETIF_DEFAULT_WIFI_STA()                                                                         \
    esp_netif_config_t {}
using QueueHandle_t = void *;
using TickType_t = uint32_t;
inline constexpr int pdTRUE = 1;
QueueHandle_t xQueueCreate(unsigned, unsigned);
void vQueueDelete(QueueHandle_t);
int xQueueSend(QueueHandle_t, const void *, TickType_t);
int xQueueReceive(QueueHandle_t, void *, TickType_t);
esp_err_t esp_event_loop_create_default();
esp_err_t esp_event_handler_instance_register(esp_event_base_t, int32_t, esp_event_handler_t, void *,
                                              void **);
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t, int32_t, void *);
esp_err_t esp_netif_init();
esp_netif_t *esp_netif_new(const esp_netif_config_t *);
void esp_netif_destroy(esp_netif_t *);
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *);
esp_err_t esp_netif_set_hostname(esp_netif_t *, const char *);
char *esp_ip4addr_ntoa(const esp_ip4_addr_t *, char *, int);
esp_err_t esp_wifi_clear_default_wifi_driver_and_handlers(esp_netif_t *);
esp_err_t esp_wifi_set_default_wifi_sta_handlers();
esp_err_t esp_wifi_init(const wifi_init_config_t *);
esp_err_t esp_wifi_deinit();
esp_err_t esp_wifi_set_storage(int);
esp_err_t esp_wifi_set_mode(int);
esp_err_t esp_wifi_set_config(int, const wifi_config_t *);
esp_err_t esp_wifi_start();
esp_err_t esp_wifi_stop();
esp_err_t esp_wifi_connect();
esp_err_t esp_wifi_disconnect();
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *);
namespace fake_wifi {
inline unsigned ap_reads{}, connects{}, stops{};
inline int8_t rssi{-50};
inline bool ap_available{true};
inline esp_netif_t *netif{};
void emit(esp_event_base_t, int, void * = nullptr);
void online();
} // namespace fake_wifi
