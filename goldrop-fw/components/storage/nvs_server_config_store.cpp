#include "goldrop/server_config_store.hpp"
#include "nvs.h"
#include "esp_log.h"

namespace goldrop {
namespace {
constexpr const char* kKey = "tcp_v2";
constexpr const char* kLegacyKey = "tcp_v1";  // Preserve for firmware rollback.
struct Handle {
    nvs_handle_t value{};
    ~Handle() { if (value) nvs_close(value); }
};
bool report(esp_err_t error) {
    if (error == ESP_OK) return true;
    ESP_LOGE("server_config", "NVS operation failed: %s; flash preserved", esp_err_to_name(error));
    return false;
}
}
ConfigLoadResult NvsServerConfigStore::load_server(ServerConfig& result) {
    result = {};
    Handle handle;
    esp_err_t error = nvs_open("goldrop", NVS_READONLY, &handle.value);
    if (error == ESP_ERR_NVS_NOT_FOUND) return ConfigLoadResult::Defaults;
    if (!report(error)) return ConfigLoadResult::Failed;
    ServerConfigBlob blob{};
    std::size_t size = blob.size();
    error = nvs_get_blob(handle.value, kKey, blob.data(), &size);
    if (error == ESP_ERR_NVS_NOT_FOUND) {
        size = blob.size();
        error = nvs_get_blob(handle.value, kLegacyKey, blob.data(), &size);
        if (error == ESP_OK && size != LegacyServerConfigBlob{}.size()) error = ESP_ERR_INVALID_SIZE;
    } else if (error == ESP_OK && (size != blob.size() || blob[4] != 2)) {
        error = ESP_ERR_INVALID_SIZE;  // A bad v2 is not silently replaced by old settings.
    }
    if (error == ESP_ERR_NVS_NOT_FOUND) return ConfigLoadResult::Defaults;
    if (!report(error) || !decode_server_config(blob.data(), size, result)) {
        ESP_LOGE("server_config", "TCP configuration unreadable; flash preserved");
        return ConfigLoadResult::Failed;
    }
    return ConfigLoadResult::Loaded;
}
bool NvsServerConfigStore::save_server(const ServerConfig& config) {
    ServerConfigBlob blob{};
    if (!encode_server_config(config, blob)) return false;
    Handle handle;
    esp_err_t error = nvs_open("goldrop", NVS_READWRITE, &handle.value);
    if (error == ESP_OK) error = nvs_set_blob(handle.value, kKey, blob.data(), blob.size());
    if (error == ESP_OK) error = nvs_commit(handle.value);
    return report(error);
}
bool NvsServerConfigStore::forget_server() {
    Handle handle;
    esp_err_t error = nvs_open("goldrop", NVS_READWRITE, &handle.value);
    if (!report(error)) return false;
    for (const char* key : {kKey, kLegacyKey}) {
        error = nvs_erase_key(handle.value, key);
        if (error != ESP_OK && error != ESP_ERR_NVS_NOT_FOUND) return report(error);
    }
    error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(handle.value);
    return report(error);
}
}  // namespace goldrop
