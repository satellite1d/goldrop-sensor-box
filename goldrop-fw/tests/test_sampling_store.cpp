#include "goldrop/nvs_sampling_config_store.hpp"
#include "nvs.h"
#include "goldrop/sampling_schedule.hpp"
#include "goldrop/boot_self_test.hpp"
#include <cassert>
#include <cstring>
using namespace goldrop;
namespace {
bool exists{}, fail_open{}, fail_get{}, fail_set{}, fail_commit{};
uint32_t durable{}, pending{};
unsigned opened{}, closed{}, writes{};
} // namespace
esp_err_t nvs_open(const char *name, int mode, nvs_handle_t *handle) {
    assert(!std::strcmp(name, "goldrop"));
    if (fail_open)
        return ESP_FAIL;
    if (!exists && mode == NVS_READONLY)
        return ESP_ERR_NVS_NOT_FOUND;
    *handle = ++opened;
    return ESP_OK;
}
esp_err_t nvs_get_u32(nvs_handle_t, const char *key, uint32_t *value) {
    assert(!std::strcmp(key, "sample_ms"));
    if (fail_get)
        return ESP_FAIL;
    if (!exists)
        return ESP_ERR_NVS_NOT_FOUND;
    *value = durable;
    return ESP_OK;
}
esp_err_t nvs_set_u32(nvs_handle_t, const char *key, uint32_t value) {
    assert(!std::strcmp(key, "sample_ms"));
    if (fail_set)
        return ESP_FAIL;
    ++writes;
    pending = value;
    return ESP_OK;
}
esp_err_t nvs_commit(nvs_handle_t) {
    if (fail_commit)
        return ESP_FAIL;
    durable = pending;
    exists = true;
    return ESP_OK;
}
void nvs_close(nvs_handle_t handle) {
    assert(handle);
    ++closed;
}
int main() {
    for (uint32_t configured : {10000u, 30000u, 60000u, 3600000u}) {
        const auto first_gap = sampling_interval_after_acquisition(configured, 1);
        assert(first_gap == 10000);
        assert(boot_self_test(true, 1, 1, 0) == BootDecision::Wait);
        assert(first_gap < settings::kBootValidationDeadlineMs);
        assert(boot_self_test(true, 2, 2, settings::kBootObservationMs) == BootDecision::Confirm);
        assert(sampling_interval_after_acquisition(configured, 2) == configured);
        assert(sampling_interval_after_acquisition(configured, 100) == configured);
        const auto trial_gap = sampling_interval_after_acquisition(configured, 100, true);
        assert(trial_gap == 10000 && trial_gap * 2 < 120000);
        assert(sampling_interval_after_acquisition(configured, 102, false) == configured);
    }
    NvsSamplingConfigStore store;
    SamplingConfig config{30000};
    assert(store.load(config) == ConfigLoadResult::Defaults && config.interval_ms == 10000);
    assert(!store.save(9999) && !store.save(3600001) && writes == 0);
    assert(store.save(60000));
    NvsSamplingConfigStore reboot;
    assert(reboot.load(config) == ConfigLoadResult::Loaded && config.interval_ms == 60000);
    fail_commit = true;
    assert(!store.save(30000));
    fail_commit = false;
    assert(reboot.load(config) == ConfigLoadResult::Loaded && config.interval_ms == 60000);
    fail_set = true;
    assert(!store.save(30000));
    fail_set = false;
    fail_get = true;
    assert(store.load(config) == ConfigLoadResult::Failed && config.interval_ms == 10000);
    fail_get = false;
    fail_open = true;
    assert(store.load(config) == ConfigLoadResult::Failed && !store.save(30000));
    fail_open = false;
    durable = 1;
    assert(store.load(config) == ConfigLoadResult::Failed && config.interval_ms == 10000);
    assert(opened == closed);
    assert(durable == 1); // Invalid existing flash is preserved, never erased.
}
