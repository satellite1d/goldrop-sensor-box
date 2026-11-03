#include "goldrop/system_clock.hpp"
#include "goldrop/settings.hpp"
#include "goldrop/utc_timestamp.hpp"
#include "esp_event.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "esp_log.h"
#include <sys/time.h>
namespace goldrop {
namespace {
bool valid_time(const timeval &time) {
    return time.tv_sec >= settings::kEarliestSynchronizedUnixSeconds &&
           time.tv_sec <= settings::kLatestSynchronizedUnixSeconds && time.tv_usec >= 0 &&
           time.tv_usec < 1'000'000;
}
} // namespace
SystemClock::~SystemClock() {
    if (handler_) {
        const auto e =
            esp_event_handler_instance_unregister(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, handler_);
        if (e != ESP_OK)
            ESP_LOGW("time", "Unregister failed: %s", esp_err_to_name(e));
    }
    if (initialized_)
        esp_netif_sntp_deinit();
}
int64_t SystemClock::uptime_ms() const { return esp_timer_get_time() / 1000; }
bool SystemClock::utc_ms(int64_t &value) const {
    if (!synchronized_.load(std::memory_order_acquire))
        return false;
    timeval now{};
    if (gettimeofday(&now, nullptr) != 0 || !valid_time(now))
        return false;
    value = int64_t(now.tv_sec) * 1000 + now.tv_usec / 1000;
    return true;
}
bool SystemClock::synchronized() const {
    int64_t now{};
    return utc_ms(now);
}
void SystemClock::sync_event(void *context, const char *, int32_t, void *data) {
    auto &self = *static_cast<SystemClock *>(context);
    const auto *sync = static_cast<esp_netif_sntp_time_sync_t *>(data);
    if (sync && valid_time(sync->tv)) {
        self.synchronized_.store(true, std::memory_order_release);
        char utc[kUtcTimestampCapacity];
        const int64_t timestamp_ms = int64_t(sync->tv.tv_sec) * 1000 + sync->tv.tv_usec / 1000;
        if (format_utc_timestamp(timestamp_ms, utc, sizeof(utc)))
            ESP_LOGI("time", "UTC synchronized by SNTP: %s", utc);
    } else {
        self.synchronized_.store(false, std::memory_order_release);
        ESP_LOGW("time", "Invalid SNTP time; measurement timestamps remain unknown");
    }
}
void SystemClock::poll(bool wifi) {
    if (wifi && !wifi_was_ready_)
        restart_pending_ = true;
    wifi_was_ready_ = wifi;
    if (!wifi || (initialized_ && !restart_pending_) || uptime_ms() < next_attempt_)
        return;
    next_attempt_ = uptime_ms() + settings::kTimeSyncRetryMs;
    if (!handler_) {
        const auto e = esp_event_handler_instance_register(NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, sync_event,
                                                           this, &handler_);
        if (e != ESP_OK) {
            ESP_LOGW("time", "Event registration failed: %s", esp_err_to_name(e));
            return;
        }
    }
    if (!initialized_) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(settings::kNtpServer);
        config.wait_for_sync = false;
        config.start = false;
        const auto e = esp_netif_sntp_init(&config);
        if (e != ESP_OK) {
            ESP_LOGW("time", "SNTP initialization failed: %s", esp_err_to_name(e));
            return;
        }
        initialized_ = true;
    }
    const auto started = esp_netif_sntp_start();
    if (started == ESP_OK) {
        restart_pending_ = false;
        ESP_LOGI("time", "SNTP synchronization requested: %s", settings::kNtpServer);
    } else
        ESP_LOGW("time", "SNTP start failed: %s", esp_err_to_name(started));
}
} // namespace goldrop
