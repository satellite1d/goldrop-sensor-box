#include "fake_wifi.hpp"
#include <array>
#include <deque>
#include <vector>
#include <cstring>
#include <cstdio>
namespace {
struct Queue {
    unsigned capacity, item_size;
    std::deque<std::vector<uint8_t>> items;
};
struct Handler {
    const char *base{};
    esp_event_handler_t callback{};
    void *context{};
};
std::array<Handler, 4> handlers{};
} // namespace
QueueHandle_t xQueueCreate(unsigned n, unsigned size) { return new Queue{n, size, {}}; }
void vQueueDelete(QueueHandle_t q) { delete static_cast<Queue *>(q); }
int xQueueSend(QueueHandle_t q, const void *item, TickType_t) {
    auto &queue = *static_cast<Queue *>(q);
    if (queue.items.size() >= queue.capacity)
        return 0;
    auto *p = static_cast<const uint8_t *>(item);
    queue.items.emplace_back(p, p + queue.item_size);
    return pdTRUE;
}
int xQueueReceive(QueueHandle_t q, void *item, TickType_t) {
    auto &queue = *static_cast<Queue *>(q);
    if (queue.items.empty())
        return 0;
    std::memcpy(item, queue.items.front().data(), queue.item_size);
    queue.items.pop_front();
    return pdTRUE;
}
esp_err_t esp_event_loop_create_default() { return ESP_OK; }
esp_err_t esp_event_handler_instance_register(const char *base, int32_t, esp_event_handler_t cb,
                                              void *context, void **out) {
    for (auto &h : handlers)
        if (!h.callback) {
            h = {base, cb, context};
            *out = &h;
            return ESP_OK;
        }
    return ESP_FAIL;
}
esp_err_t esp_event_handler_instance_unregister(const char *, int32_t, void *h) {
    *static_cast<Handler *>(h) = {};
    return ESP_OK;
}
void fake_wifi::emit(const char *base, int id, void *data) {
    for (auto &h : handlers)
        if (h.callback && h.base == base)
            h.callback(h.context, base, id, data);
}
void fake_wifi::online() {
    emit(WIFI_EVENT, WIFI_EVENT_STA_CONNECTED);
    ip_event_got_ip_t ip;
    ip.esp_netif = netif;
    ip.ip_info.ip.addr = 1;
    emit(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip);
}
esp_err_t esp_netif_init() { return ESP_OK; }
esp_netif_t *esp_netif_new(const esp_netif_config_t *) {
    fake_wifi::netif = new esp_netif_t;
    return fake_wifi::netif;
}
void esp_netif_destroy(esp_netif_t *p) {
    delete p;
    fake_wifi::netif = nullptr;
}
esp_err_t esp_netif_attach_wifi_station(esp_netif_t *) { return ESP_OK; }
esp_err_t esp_netif_set_hostname(esp_netif_t *, const char *) { return ESP_OK; }
char *esp_ip4addr_ntoa(const esp_ip4_addr_t *, char *out, int capacity) {
    std::snprintf(out, capacity, "192.0.2.1");
    return out;
}
esp_err_t esp_wifi_clear_default_wifi_driver_and_handlers(esp_netif_t *) { return ESP_OK; }
esp_err_t esp_wifi_set_default_wifi_sta_handlers() { return ESP_OK; }
esp_err_t esp_wifi_init(const wifi_init_config_t *) { return ESP_OK; }
esp_err_t esp_wifi_deinit() { return ESP_OK; }
esp_err_t esp_wifi_set_storage(int) { return ESP_OK; }
esp_err_t esp_wifi_set_mode(int) { return ESP_OK; }
esp_err_t esp_wifi_set_config(int, const wifi_config_t *) { return ESP_OK; }
esp_err_t esp_wifi_start() {
    fake_wifi::emit(WIFI_EVENT, WIFI_EVENT_STA_START);
    return ESP_OK;
}
esp_err_t esp_wifi_stop() {
    ++fake_wifi::stops;
    fake_wifi::emit(WIFI_EVENT, WIFI_EVENT_STA_STOP);
    return ESP_OK;
}
esp_err_t esp_wifi_connect() {
    ++fake_wifi::connects;
    return ESP_OK;
}
esp_err_t esp_wifi_disconnect() {
    wifi_event_sta_disconnected_t e{1};
    fake_wifi::emit(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &e);
    return ESP_OK;
}
esp_err_t esp_wifi_sta_get_ap_info(wifi_ap_record_t *ap) {
    ++fake_wifi::ap_reads;
    ap->rssi = fake_wifi::rssi;
    return fake_wifi::ap_available ? ESP_OK : ESP_FAIL;
}
