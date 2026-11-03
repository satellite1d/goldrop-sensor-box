#include "goldrop/nvs_sampling_config_store.hpp"
#include "nvs.h"
#include "esp_log.h"
namespace goldrop {
namespace {
struct Handle {
    nvs_handle_t value{};
    ~Handle() {
        if (value)
            nvs_close(value);
    }
};
} // namespace
ConfigLoadResult NvsSamplingConfigStore::load(SamplingConfig &out) {
    out = {};
    Handle h;
    auto e = nvs_open("goldrop", NVS_READONLY, &h.value);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return ConfigLoadResult::Defaults;
    if (e == ESP_OK)
        e = nvs_get_u32(h.value, "sample_ms", &out.interval_ms);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return ConfigLoadResult::Defaults;
    if (e != ESP_OK || !out.valid()) {
        out = {};
        ESP_LOGE("sampling", "Cannot read interval; preserving NVS");
        return ConfigLoadResult::Failed;
    }
    return ConfigLoadResult::Loaded;
}
bool NvsSamplingConfigStore::save(uint32_t interval_ms) {
    if (!SamplingConfig{interval_ms}.valid())
        return false;
    Handle h;
    auto e = nvs_open("goldrop", NVS_READWRITE, &h.value);
    if (e == ESP_OK)
        e = nvs_set_u32(h.value, "sample_ms", interval_ms);
    if (e == ESP_OK)
        e = nvs_commit(h.value);
    if (e != ESP_OK)
        ESP_LOGE("sampling", "Cannot persist interval: %s", esp_err_to_name(e));
    return e == ESP_OK;
}
} // namespace goldrop
