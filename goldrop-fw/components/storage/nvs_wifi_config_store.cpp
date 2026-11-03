#include "goldrop/nvs_wifi_config_store.hpp"
#include "goldrop/wifi_config_codec.hpp"
#include "nvs.h"
#include "esp_log.h"

namespace goldrop {
namespace {
constexpr const char* kTag = "wifi_config";
constexpr const char* kNamespace = "goldrop";
constexpr const char* kKey = "wifi_v1";
class NvsHandle {
public:
    ~NvsHandle() { if (handle) nvs_close(handle); }
    nvs_handle_t handle{};
};
bool report(esp_err_t result) {
    if (result == ESP_OK) return true;
    ESP_LOGE(kTag, "NVS operation failed: %s; flash preserved", esp_err_to_name(result));
    return false;
}
}  // namespace

ConfigLoadResult NvsWifiConfigStore::load_wifi(WifiCredentials& result) {
    result.clear();
    NvsHandle nvs;
    esp_err_t error = nvs_open(kNamespace, NVS_READONLY, &nvs.handle);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ConfigLoadResult::Defaults;
    if (!report(error)) return ConfigLoadResult::Failed;
    WifiConfigBlob blob{};
    std::size_t size = blob.size();
    error = nvs_get_blob(nvs.handle, kKey, blob.data(), &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ConfigLoadResult::Defaults;
    const bool valid = report(error) && decode_wifi_config(blob.data(), size, result);
    clear_secret(blob.data(), blob.size());
    if (!valid) {
        ESP_LOGE(kTag, "Wi-Fi configuration unreadable; USB setup available, flash preserved");
        return ConfigLoadResult::Failed;
    }
    return ConfigLoadResult::Loaded;
}

bool NvsWifiConfigStore::save_wifi(const WifiCredentials& config) {
    WifiConfigBlob blob{};
    if (!encode_wifi_config(config, blob)) return false;
    NvsHandle nvs;
    esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &nvs.handle);
    if (result == ESP_OK) result = nvs_set_blob(nvs.handle, kKey, blob.data(), blob.size());
    if (result == ESP_OK) result = nvs_commit(nvs.handle);
    clear_secret(blob.data(), blob.size());
    return report(result);
}

bool NvsWifiConfigStore::forget_wifi() {
    NvsHandle nvs;
    esp_err_t result = nvs_open(kNamespace, NVS_READWRITE, &nvs.handle);
    if (result == ESP_OK) result = nvs_erase_key(nvs.handle, kKey);
    if (result == ESP_ERR_NVS_NOT_FOUND) return true;
    if (result == ESP_OK) result = nvs_commit(nvs.handle);
    return report(result);
}
}  // namespace goldrop
