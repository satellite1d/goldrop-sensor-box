#include "goldrop/ota_config_store.hpp"
#include "goldrop/binary_codec.hpp"
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
bool report(esp_err_t e) {
    if (e == ESP_OK)
        return true;
    ESP_LOGE("ota_config", "NVS error: %s", esp_err_to_name(e));
    return false;
}
bool write(const char *key, const void *data, std::size_t size) {
    Handle h;
    auto e = nvs_open("goldrop", NVS_READWRITE, &h.value);
    if (e == ESP_OK)
        e = nvs_set_blob(h.value, key, data, size);
    if (e == ESP_OK)
        e = nvs_commit(h.value);
    return report(e);
}
bool erase(const char *key) {
    Handle h;
    auto e = nvs_open("goldrop", NVS_READWRITE, &h.value);
    if (e == ESP_OK)
        e = nvs_erase_key(h.value, key);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return true;
    if (e == ESP_OK)
        e = nvs_commit(h.value);
    return report(e);
}
} // namespace
ConfigLoadResult NvsOtaConfigStore::load_ota(OtaConfig &c) {
    c = {};
    Handle h;
    auto e = nvs_open("goldrop", NVS_READONLY, &h.value);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return ConfigLoadResult::Defaults;
    if (!report(e))
        return ConfigLoadResult::Failed;
    OtaConfigBlob blob;
    std::size_t n = blob.size();
    e = nvs_get_blob(h.value, "ota_v2", blob.data(), &n);
    if (e == ESP_ERR_NVS_NOT_FOUND) {
        n = blob.size();
        e = nvs_get_blob(h.value, "ota_v1", blob.data(), &n);
    }
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return ConfigLoadResult::Defaults;
    return report(e) && decode_ota_config(blob.data(), n, c) ? ConfigLoadResult::Loaded
                                                             : ConfigLoadResult::Failed;
}
bool NvsOtaConfigStore::save_ota(const OtaConfig &c) {
    OtaConfigBlob b;
    return encode_ota_config(c, b) && write("ota_v2", b.data(), b.size());
}
bool NvsOtaConfigStore::forget_ota() {
    Handle h;
    auto e = nvs_open("goldrop", NVS_READWRITE, &h.value);
    if (!report(e))
        return false;
    for (const char *key : {"ota_v2", "ota_v1"}) {
        e = nvs_erase_key(h.value, key);
        if (e != ESP_OK && e != ESP_ERR_NVS_NOT_FOUND)
            return report(e);
    }
    return report(nvs_commit(h.value));
}
bool OtaAttemptStore::load(OtaAttempt &a) {
    a = {};
    Handle h;
    auto e = nvs_open("goldrop", NVS_READONLY, &h.value);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return true;
    if (!report(e))
        return false;
    std::array<uint8_t, 76> b{};
    std::size_t n = b.size();
    e = nvs_get_blob(h.value, "ota_attempt_v1", b.data(), &n);
    if (e == ESP_ERR_NVS_NOT_FOUND)
        return true;
    if (!report(e) || n != b.size() || std::memcmp(b.data(), "GOT1", 4) || b[4] > 1 || b[5] > 1 || b[6] ||
        b[7] || binary::get32(b.data() + 72) != binary::crc32(b.data(), 72) ||
        !std::memchr(b.data() + 40, 0, 32))
        return false;
    a.pending = b[4];
    a.blocked = b[5];
    std::memcpy(a.hash.data(), b.data() + 8, 32);
    std::memcpy(a.version, b.data() + 40, 32);
    return true;
}
bool OtaAttemptStore::save(const OtaAttempt &a) {
    if (!std::memchr(a.version, 0, 32))
        return false;
    std::array<uint8_t, 76> b{};
    std::memcpy(b.data(), "GOT1", 4);
    b[4] = a.pending;
    b[5] = a.blocked;
    std::memcpy(b.data() + 8, a.hash.data(), 32);
    std::memcpy(b.data() + 40, a.version, 32);
    binary::put32(b.data() + 72, binary::crc32(b.data(), 72));
    return write("ota_attempt_v1", b.data(), b.size());
}
bool OtaAttemptStore::clear() { return erase("ota_attempt_v1"); }
} // namespace goldrop
