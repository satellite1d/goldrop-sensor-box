#include "goldrop/wifi_manager.hpp"
#include "goldrop/settings.hpp"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include <atomic>
#include <cstring>
#include <new>

namespace goldrop {
namespace {
constexpr const char* kTag = "wifi";
constexpr unsigned kEventCapacity = 16;
enum class EventKind { Started, Stopped, Associated, Disconnected, GotIp, LostIp };
struct Event { EventKind kind{}; uint16_t reason{}; esp_ip4_addr_t ip{}; };
void cleanup_result(esp_err_t result) {
    if (result != ESP_OK) ESP_LOGW(kTag, "Cleanup failed: %s", esp_err_to_name(result));
}
}  // namespace

class WifiManager::Impl {
public:
    ~Impl() {
        if (wifi_handler_) cleanup_result(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_handler_));
        if (ip_handler_) cleanup_result(esp_event_handler_instance_unregister(IP_EVENT, ESP_EVENT_ANY_ID, ip_handler_));
        if (driver_started_) cleanup_result(esp_wifi_stop());
        if (driver_initialized_) cleanup_result(esp_wifi_deinit());
        if (netif_) {
            cleanup_result(esp_wifi_clear_default_wifi_driver_and_handlers(netif_));
            esp_netif_destroy(netif_);
        }
        if (events_) vQueueDelete(events_);
        desired_.clear();
    }

    bool configure(const WifiCredentials& config, int64_t now) {
        if (!config.valid()) return false;
        desired_.clear();
        desired_ = config;
        restart_with_config(now);
        return true;  // Accepted; association and DHCP complete asynchronously.
    }
    void disable(int64_t now) {
        desired_.clear();
        restart_with_config(now);
    }
    NetworkStatus status() const {
        NetworkStatus result;
        result.state = policy_.state();
        if (result.state == WifiState::Online) {
            esp_ip4addr_ntoa(&ip_, result.ip, sizeof(result.ip));
            result.rssi_dbm = rssi_dbm_;
            result.rssi_valid = rssi_valid_;
        }
        return result;
    }
    void poll(int64_t now) {
        if (overflow_.exchange(false, std::memory_order_relaxed)) {
            ESP_LOGW(kTag, "Event queue overflow; reconciling by restarting Wi-Fi interface");
            restart_with_config(now);
        }
        Event event;
        // Bound callback processing, even if the network generates an event storm.
        for (unsigned i = 0; events_ && i < kEventCapacity && xQueueReceive(events_, &event, 0) == pdTRUE; ++i) {
            handle(event, now);
        }
        const WifiAction action = policy_.poll(now, esp_random());
        if (action == WifiAction::Retry) {
            if (!desired_.valid()) { policy_.reset(); return; }
            if (!driver_started_) start_driver(now);
            else {
                ESP_LOGI(kTag, "Connecting to configured network");
                const esp_err_t result = esp_wifi_connect();
                if (result != ESP_OK) failed(result, now);
            }
        } else if (action == WifiAction::Disconnect) {
            ip_ = {};
            ESP_LOGW(kTag, "Connection/DHCP deadline expired; retry in %lu ms",
                     static_cast<unsigned long>(policy_.retry_delay_ms(now)));
            const esp_err_t result = esp_wifi_disconnect();
            if (result != ESP_OK) ESP_LOGW(kTag, "Disconnect failed: %s", esp_err_to_name(result));
        } else if (action == WifiAction::Stop) {
            ip_ = {};
            stop_for_config_ = false;
            const esp_err_t result = driver_initialized_ ? esp_wifi_stop() : ESP_OK;
            if (result != ESP_OK && result != ESP_ERR_WIFI_NOT_STARTED) {
                ESP_LOGW(kTag, "Stop recovery failed: %s; retaining stop state", esp_err_to_name(result));
                stop_for_config_ = true;
                policy_.stopping(now);
                return;
            }
            driver_started_ = false;
            if (!desired_.valid()) policy_.reset();
            ESP_LOGW(kTag, "Wi-Fi start/stop deadline expired; recovery scheduled");
        }
        if (policy_.state() != WifiState::Online) {
            rssi_valid_ = false;
            next_rssi_sample_ms_ = 0;
        } else if (now >= next_rssi_sample_ms_) {
            wifi_ap_record_t ap{};
            rssi_valid_ = esp_wifi_sta_get_ap_info(&ap) == ESP_OK;
            rssi_dbm_ = rssi_valid_ ? ap.rssi : 0;
            next_rssi_sample_ms_ = now + settings::kWifiRssiPollMs;
        }
    }
private:
    static void event_callback(void* context, esp_event_base_t base, int32_t id, void* data) {
        auto& self = *static_cast<Impl*>(context);
        Event event;
        if (base == WIFI_EVENT) {
            switch (id) {
                case WIFI_EVENT_STA_START: event.kind = EventKind::Started; break;
                case WIFI_EVENT_STA_STOP: event.kind = EventKind::Stopped; break;
                case WIFI_EVENT_STA_CONNECTED: event.kind = EventKind::Associated; break;
                case WIFI_EVENT_STA_DISCONNECTED:
                    event.kind = EventKind::Disconnected;
                    event.reason = static_cast<const wifi_event_sta_disconnected_t*>(data)->reason;
                    break;
                default: return;
            }
        } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
            const auto* ip = static_cast<const ip_event_got_ip_t*>(data);
            if (ip->esp_netif != self.netif_) return;
            event.kind = EventKind::GotIp;
            event.ip = ip->ip_info.ip;
        } else if (base == IP_EVENT && id == IP_EVENT_STA_LOST_IP) {
            event.kind = EventKind::LostIp;
        } else return;
        if (xQueueSend(self.events_, &event, 0) != pdTRUE) {
            self.overflow_.store(true, std::memory_order_relaxed);
        }
    }

    esp_err_t initialize() {
        if (!events_) {
            events_ = xQueueCreate(kEventCapacity, sizeof(Event));
            if (!events_) return ESP_ERR_NO_MEM;
        }
        esp_err_t result = esp_netif_init();
        if (result != ESP_OK) return result;
        result = esp_event_loop_create_default();
        if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) return result;
        if (!netif_) {
            const esp_netif_config_t config = ESP_NETIF_DEFAULT_WIFI_STA();
            netif_ = esp_netif_new(&config);
            if (!netif_) return ESP_ERR_NO_MEM;
        }
        if (!attached_) {
            result = esp_netif_attach_wifi_station(netif_);
            if (result != ESP_OK) return result;
            attached_ = true;
        }
        if (!default_handlers_) {
            result = esp_wifi_set_default_wifi_sta_handlers();
            if (result != ESP_OK) return result;
            default_handlers_ = true;
        }
        if (!wifi_handler_) {
            result = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, event_callback, this, &wifi_handler_);
            if (result != ESP_OK) return result;
        }
        if (!ip_handler_) {
            result = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID, event_callback, this, &ip_handler_);
            if (result != ESP_OK) return result;
        }
        if (!driver_initialized_) {
            wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
            config.nvs_enable = false;  // Our versioned ConfigStore is the only credential owner.
            result = esp_wifi_init(&config);
            if (result != ESP_OK) return result;
            driver_initialized_ = true;
        }
        result = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (result != ESP_OK) return result;
        result = esp_wifi_set_mode(WIFI_MODE_STA);
        if (result != ESP_OK) return result;
        return esp_netif_set_hostname(netif_, "goldrop");
    }

    void start_driver(int64_t now) {
        if (!desired_.valid()) { policy_.reset(); return; }
        policy_.starting(now);
        esp_err_t result = initialize();
        if (result != ESP_OK) { failed(result, now); return; }
        wifi_config_t config{};
        std::memcpy(config.sta.ssid, desired_.ssid.data(), std::strlen(desired_.ssid.data()));
        std::memcpy(config.sta.password, desired_.password.data(), std::strlen(desired_.password.data()));
        config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        config.sta.pmf_cfg.capable = true;
        config.sta.pmf_cfg.required = false;
        config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
        config.sta.failure_retry_cnt = 0;  // Application owns reconnect/backoff.
        result = esp_wifi_set_config(WIFI_IF_STA, &config);
        clear_secret(&config, sizeof(config));
        if (result == ESP_OK) result = esp_wifi_start();
        if (result != ESP_OK) { failed(result, now); return; }
        driver_started_ = true;
        ESP_LOGI(kTag, "Station starting (2.4 GHz, WPA2/WPA3-Personal)");
    }

    void restart_with_config(int64_t now) {
        ip_ = {};
        rssi_valid_ = false;
        next_rssi_sample_ms_ = 0;
        if (stop_for_config_) return;  // Latest desired configuration wins at the STOP boundary.
        policy_.reset();
        if (driver_started_) {
            stop_for_config_ = true;
            policy_.stopping(now);
            const esp_err_t result = esp_wifi_stop();
            if (result == ESP_OK) return;
            ESP_LOGW(kTag, "Stop for reconfiguration failed: %s", esp_err_to_name(result));
            // Keep the stop deadline; retry recovery without assuming the interface stopped.
            return;
        }
        start_driver(now);
    }
    void failed(esp_err_t error, int64_t now) {
        policy_.failed(now, esp_random());
        ip_ = {};
        ESP_LOGW(kTag, "Operation failed: %s; retry in %lu ms", esp_err_to_name(error),
                 static_cast<unsigned long>(policy_.retry_delay_ms(now)));
    }
    void handle(const Event& event, int64_t now) {
        switch (event.kind) {
            case EventKind::Started: policy_.started(now); break;
            case EventKind::Stopped:
                driver_started_ = false;
                if (stop_for_config_) {
                    stop_for_config_ = false;
                    start_driver(now);
                }
                break;
            case EventKind::Associated:
                policy_.associated(now);
                break;
            case EventKind::GotIp:
                if (event.ip.addr && policy_.got_ip(now)) {
                    ip_ = event.ip;
                    char address[16];
                    esp_ip4addr_ntoa(&ip_, address, sizeof(address));
                    ESP_LOGI(kTag, "Connected; IP=%s", address);
                }
                break;
            case EventKind::Disconnected:
                ip_ = {};
                policy_.failed(now, esp_random());
                if (policy_.state() == WifiState::Backoff) {
                    ESP_LOGW(kTag, "Disconnected, reason=%u; retry in %lu ms", event.reason,
                             static_cast<unsigned long>(policy_.retry_delay_ms(now)));
                }
                break;
            case EventKind::LostIp:
                if (policy_.state() == WifiState::Online) {
                    policy_.failed(now, esp_random());
                    ip_ = {};
                    const esp_err_t result = esp_wifi_disconnect();
                    if (result != ESP_OK) ESP_LOGW(kTag, "IP loss recovery failed: %s", esp_err_to_name(result));
                    ESP_LOGW(kTag, "IP lost; reconnect scheduled");
                }
                break;
        }
    }

    WifiCredentials desired_;
    WifiRetryPolicy policy_;
    esp_ip4_addr_t ip_{};
    QueueHandle_t events_{};
    std::atomic<bool> overflow_{false};
    esp_netif_t* netif_{};
    esp_event_handler_instance_t wifi_handler_{};
    esp_event_handler_instance_t ip_handler_{};
    bool attached_{};
    bool default_handlers_{};
    bool driver_initialized_{};
    bool driver_started_{};
    bool stop_for_config_{};
    bool rssi_valid_{};
    int rssi_dbm_{};
    int64_t next_rssi_sample_ms_{};
};

WifiManager::WifiManager() = default;
WifiManager::~WifiManager() = default;
bool WifiManager::configure(const WifiCredentials& config, int64_t now_ms) {
    if (!config.valid()) return false;
    if (!impl_) impl_.reset(new (std::nothrow) Impl);
    return impl_ && impl_->configure(config, now_ms);
}
void WifiManager::disable(int64_t now_ms) { if (impl_) impl_->disable(now_ms); }
NetworkStatus WifiManager::status() const { return impl_ ? impl_->status() : NetworkStatus{}; }
void WifiManager::poll(int64_t now_ms) { if (impl_) impl_->poll(now_ms); }
}  // namespace goldrop
