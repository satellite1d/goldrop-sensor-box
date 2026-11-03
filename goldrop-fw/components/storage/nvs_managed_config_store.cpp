#include "goldrop/managed_config_store.hpp"
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
ConfigLoadResult NvsManagedConfigStore::load(ManagedConfigState &state) {
    state.clear();
    Handle h;
    auto e = nvs_open("goldrop", NVS_READONLY, &h.value);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return ConfigLoadResult::Defaults;
    if (e != ESP_OK) {
        ESP_LOGE("config_store", "NVS open failed: %s", esp_err_to_name(e));
        return ConfigLoadResult::Failed;
    }
    ManagedConfigBlob blob{};
    std::size_t size = blob.size();
    e = nvs_get_blob(h.value, "cfg_txn_v1", blob.data(), &size);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return ConfigLoadResult::Defaults;
    const bool ok = e == ESP_OK && decode_managed_config(blob.data(), size, state);
    clear_secret(blob.data(), blob.size());
    if (!ok)
        ESP_LOGE("config_store", "Configuration unreadable; preserving NVS");
    return ok ? ConfigLoadResult::Loaded : ConfigLoadResult::Failed;
}
bool NvsManagedConfigStore::save(const ManagedConfigState &state) {
    ManagedConfigBlob blob{};
    if (!encode_managed_config(state, blob))
        return false;
    Handle h;
    auto e = nvs_open("goldrop", NVS_READWRITE, &h.value);
    if (e == ESP_OK)
        e = nvs_set_blob(h.value, "cfg_txn_v1", blob.data(), blob.size());
    if (e == ESP_OK)
        e = nvs_commit(h.value);
    clear_secret(blob.data(), blob.size());
    if (e != ESP_OK)
        ESP_LOGE("config_store", "NVS commit failed: %s", esp_err_to_name(e));
    return e == ESP_OK;
}
} // namespace goldrop
