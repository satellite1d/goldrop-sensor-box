#include "goldrop/system_clock.hpp"
#include "goldrop/settings.hpp"
#include "esp_netif_sntp.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
namespace {
int64_t uptime{};
timeval wall{1'792'953'730, 123456};
int wall_result{};
unsigned registrations{}, unregisters{}, initializations{}, starts{}, deinits{};
unsigned registration_failures{}, initialization_failures{}, start_failures{};
esp_event_handler_t handler{};
void *context{};
int handle_token;
void require(bool value, int line) {
    if (!value) {
        std::fprintf(stderr, "FAIL clock test line %d\n", line);
        std::exit(1);
    }
}
#define REQUIRE(...) require((__VA_ARGS__), __LINE__)
void reset() {
    uptime = wall_result = 0;
    registrations = unregisters = initializations = starts = deinits = 0;
    registration_failures = initialization_failures = start_failures = 0;
    handler = nullptr;
    context = nullptr;
    wall = {1'792'953'730, 123456};
}
void synchronize() {
    REQUIRE(handler && context);
    esp_netif_sntp_time_sync_t event{wall};
    handler(context, NETIF_SNTP_EVENT, NETIF_SNTP_TIME_SYNC, &event);
}
} // namespace
int64_t esp_timer_get_time() { return uptime * 1000; }
extern "C" int gettimeofday(timeval *output, void *) noexcept {
    *output = wall;
    return wall_result;
}
esp_err_t esp_event_handler_instance_register(esp_event_base_t base, int32_t id, esp_event_handler_t callback,
                                              void *argument, esp_event_handler_instance_t *instance) {
    REQUIRE(base == NETIF_SNTP_EVENT && id == NETIF_SNTP_TIME_SYNC);
    ++registrations;
    if (registration_failures) {
        --registration_failures;
        return ESP_FAIL;
    }
    handler = callback;
    context = argument;
    *instance = &handle_token;
    return ESP_OK;
}
esp_err_t esp_event_handler_instance_unregister(esp_event_base_t, int32_t,
                                                esp_event_handler_instance_t instance) {
    REQUIRE(instance == &handle_token);
    ++unregisters;
    handler = nullptr;
    context = nullptr;
    return ESP_OK;
}
esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) {
    REQUIRE(!config->wait_for_sync && !config->start);
    REQUIRE(std::strcmp(config->server, goldrop::settings::kNtpServer) == 0);
    ++initializations;
    if (initialization_failures) {
        --initialization_failures;
        return ESP_FAIL;
    }
    return ESP_OK;
}
esp_err_t esp_netif_sntp_start() {
    ++starts;
    if (start_failures) {
        --start_failures;
        return ESP_FAIL;
    }
    return ESP_OK;
}
void esp_netif_sntp_deinit() { ++deinits; }
int main() {
    using goldrop::SystemClock;
    reset();
    {
        SystemClock clock;
        int64_t now{};
        REQUIRE(!clock.utc_ms(now) && !clock.synchronized());
        clock.poll(false);
        REQUIRE(registrations == 0);
        clock.poll(true);
        REQUIRE(registrations == 1 && initializations == 1 && starts == 1);
        REQUIRE(!clock.utc_ms(now));
        synchronize();
        REQUIRE(clock.utc_ms(now) && clock.synchronized() && now == 1'792'953'730'123);
        uptime = 5000;
        wall.tv_sec += 5;
        clock.poll(false);
        REQUIRE(clock.utc_ms(now) && now == 1'792'953'735'123);
        clock.poll(true);
        REQUIRE(starts == 1); // A brief Wi-Fi flap cannot flood NTP.
        uptime = goldrop::settings::kTimeSyncRetryMs;
        clock.poll(true);
        REQUIRE(starts == 2 && initializations == 1);
        clock.poll(true);
        REQUIRE(starts == 2);
        wall_result = -1;
        REQUIRE(!clock.utc_ms(now) && !clock.synchronized());
        wall_result = 0;
        wall.tv_sec = 0;
        REQUIRE(!clock.synchronized());
        synchronize();
        wall = {1'792'953'730, 123456};
        REQUIRE(!clock.synchronized()); // An invalid sync revoked the source until a good event.
        synchronize();
        REQUIRE(clock.synchronized());
        wall.tv_sec = goldrop::settings::kLatestSynchronizedUnixSeconds + 1;
        synchronize();
        REQUIRE(!clock.synchronized());
        wall.tv_sec = goldrop::settings::kEarliestSynchronizedUnixSeconds;
        synchronize();
        REQUIRE(clock.synchronized());
        wall.tv_usec = 1'000'000;
        REQUIRE(!clock.utc_ms(now));
        synchronize();
        REQUIRE(!clock.synchronized());
    }
    REQUIRE(unregisters == 1 && deinits == 1 && !handler);
    reset();
    {
        registration_failures = 1;
        initialization_failures = 1;
        start_failures = 1;
        SystemClock clock;
        clock.poll(true);
        clock.poll(true);
        REQUIRE(registrations == 1 && initializations == 0);
        uptime = 60'000;
        clock.poll(true);
        REQUIRE(registrations == 2 && initializations == 1 && starts == 0);
        uptime = 120'000;
        clock.poll(true);
        REQUIRE(initializations == 2 && starts == 1);
        uptime = 179'999;
        clock.poll(true);
        REQUIRE(starts == 1);
        uptime = 180'000;
        clock.poll(true);
        REQUIRE(initializations == 2 && registrations == 2 && starts == 2);
        REQUIRE(!clock.synchronized());
        synchronize();
        REQUIRE(clock.synchronized());
    }
    REQUIRE(unregisters == 1 && deinits == 1);
    std::puts("PASS: async SNTP, valid time, offline clock, reconnect, invalid time and bounded retries");
}
