#include "goldrop/ota_manager.hpp"
#include "goldrop/ota_image_stream.hpp"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_ota_ops.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include <memory>
#include <new>
#include <cstring>
#include <algorithm>
namespace goldrop {
namespace {
int64_t uptime() { return esp_timer_get_time() / 1000; }
struct Http {
    esp_http_client_handle_t value{};
    ~Http() {
        if (value) {
            const auto e = esp_http_client_cleanup(value);
            if (e != ESP_OK)
                ESP_LOGW("ota", "HTTP cleanup: %s", esp_err_to_name(e));
        }
    }
    bool open(const char *url, const char *device) {
        HttpUrl endpoint;
        if (!endpoint.parse(url))
            return false;
        esp_http_client_config_t c{};
        c.url = url;
        c.crt_bundle_attach = endpoint.tls ? esp_crt_bundle_attach : nullptr;
        c.timeout_ms = settings::kOtaIoTimeoutMs;
        c.disable_auto_redirect = true;
        c.transport_type = endpoint.tls ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP;
        c.buffer_size = 2048;
        c.buffer_size_tx = 1024;
        value = esp_http_client_init(&c);
        if (!value)
            return false;
        if (esp_http_client_set_header(value, "Accept-Encoding", "identity") != ESP_OK ||
            esp_http_client_set_header(value, "X-Goldrop-Device", device) != ESP_OK ||
            esp_http_client_set_header(value, "Cache-Control", "no-cache") != ESP_OK)
            return false;
        const auto opened = esp_http_client_open(value, 0);
        if (opened != ESP_OK) {
            ESP_LOGW("ota", "HTTP(S) connection failed: %s", esp_err_to_name(opened));
            return false;
        }
        if (esp_http_client_fetch_headers(value) < 0) {
            ESP_LOGW("ota", "Could not read HTTP(S) response headers");
            return false;
        }
        // Explicit HTTP streaming: redirects are never followed, including by an OTA helper.
        const auto status = esp_http_client_get_status_code(value);
        if (status != 200)
            ESP_LOGW("ota", "HTTP(S) response rejected: HTTP %d", status);
        return status == 200;
    }
};
struct Download {
    esp_ota_handle_t handle{};
    bool open{};
    ~Download() {
        if (open) {
            const auto e = esp_ota_abort(handle);
            if (e != ESP_OK)
                ESP_LOGW("ota", "Abort: %s", esp_err_to_name(e));
        }
    }
};
struct ImageWriter final : OtaImageWriter {
    explicit ImageWriter(esp_ota_handle_t value) : handle(value) {}
    bool write(const uint8_t *data, std::size_t size) override {
        return esp_ota_write(handle, data, size) == ESP_OK;
    }
    esp_ota_handle_t handle;
};
struct Hash {
    psa_hash_operation_t value = PSA_HASH_OPERATION_INIT;
    ~Hash() {
        const auto e = psa_hash_abort(&value);
        if (e != PSA_SUCCESS)
            ESP_LOGW("ota", "Hash cleanup failed");
    }
};
} // namespace
OtaManager::~OtaManager() {
    cancel();
    while (active_.load(std::memory_order_acquire))
        vTaskDelay(1);
}
bool OtaManager::begin() {
    signature_.begin(); // An absent key disables signed HTTP; HTTPS remains available.
    initialized_ = attempts_.load(attempt_);
    if (!initialized_) {
        fail("attempt_storage");
        return false;
    }
    if (attempt_.pending && std::strcmp(attempt_.version, kFirmwareVersion) != 0) {
        // Conservatively block interrupted activation/rollback; a local retry is available.
        attempt_.pending = false;
        attempt_.blocked = true;
        initialized_ = attempts_.save(attempt_);
        ESP_LOGW("ota", "Previous activation failed or was interrupted; release blocked");
        if (!initialized_)
            fail("attempt_block_storage");
    }
    return initialized_;
}
bool OtaManager::configure_ota(const OtaConfig &c, int64_t now) {
    HttpUrl endpoint;
    if (!c.valid() || !endpoint.parse(c.manifest_url.data()) || (!endpoint.tls && !signature_.available()) ||
        state() == OtaState::ReadyToReboot)
        return false;
    cancel();
    config_ = c;
    configured_ = true;
    requested_ = false;
    next_check_ = now;
    if (!active_.load())
        state_.store(initialized_ ? (c.enabled ? OtaState::Idle : OtaState::Disabled) : OtaState::Failed);
    return true;
}
void OtaManager::disable_ota() {
    if (state() == OtaState::ReadyToReboot)
        return; // Activation is already committed.
    cancel();
    configured_ = false;
    requested_ = false;
    if (!active_.load())
        state_.store(OtaState::NotConfigured);
}
bool OtaManager::request_check() {
    if (!initialized_ || !configured_ || active_.load() || state() == OtaState::ReadyToReboot)
        return false;
    requested_ = true;
    next_check_ = 0;
    return true;
}
bool OtaManager::clear_block() {
    if (active_.load() || state() == OtaState::ReadyToReboot)
        return false;
    if (!attempts_.clear())
        return false;
    attempt_ = {};
    initialized_ = true;
    return true;
}
bool OtaManager::healthy_request() const {
    return !cancel_.load(std::memory_order_acquire) && network_.load(std::memory_order_acquire) &&
           storage_.load(std::memory_order_acquire);
}
void OtaManager::fail(const char *reason) {
    error_.store(reason, std::memory_order_release);
    state_.store(OtaState::Failed, std::memory_order_release);
    ESP_LOGW("ota", "Update failed: %s", reason);
}
void OtaManager::poll(int64_t now, bool network, bool time, bool boot, bool storage,
                      bool configuration_ready) {
    network_.store(network, std::memory_order_release);
    storage_.store(storage, std::memory_order_release);
    if (active_.load(std::memory_order_acquire) || state() == OtaState::ReadyToReboot)
        return;
    const unsigned completed = completions_.load(std::memory_order_acquire);
    if (completed != observed_completion_) {
        observed_completion_ = completed;
        if (!attempts_.load(attempt_)) {
            initialized_ = false;
            fail("attempt_storage");
            return;
        }
        if (success_.load()) {
            backoff_.reset();
            next_check_ = now + int64_t(config_.interval_seconds) * 1000 + esp_random() % 60001;
        } else
            next_check_ = now + backoff_.next_delay_ms();
    }
    if (!initialized_)
        return;
    if (boot && !attempt_reconciled_) {
        if (now < next_reconcile_)
            return;
        if (attempt_.pending && std::strcmp(attempt_.version, kFirmwareVersion) == 0) {
            if (!attempts_.clear()) {
                next_reconcile_ = now + 60'000;
                fail("attempt_confirm_storage");
                return;
            }
            attempt_ = {};
        }
        attempt_reconciled_ = true;
    }
    if (!configured_) {
        state_.store(OtaState::NotConfigured);
        return;
    }
    if (!requested_ && !config_.enabled) {
        state_.store(OtaState::Disabled);
        return;
    }
    if (!boot) {
        state_.store(OtaState::WaitingBoot);
        return;
    }
    if (!network) {
        state_.store(OtaState::WaitingNetwork);
        return;
    }
    HttpUrl endpoint;
    if (!endpoint.parse(config_.manifest_url.data())) {
        fail("manifest_url");
        return;
    }
    if (endpoint.tls && !time) {
        state_.store(OtaState::WaitingTime);
        return;
    }
    if (!storage) {
        state_.store(OtaState::WaitingStorage);
        return;
    }
    if (now < next_check_)
        return;
    if (!configuration_ready) {
        state_.store(OtaState::WaitingConfiguration);
        return;
    }
    requested_ = false;
    work_config_ = config_;
    work_attempt_ = attempt_;
    cancel_.store(false);
    restart_allowed_.store(false);
    progress_.store(0);
    error_.store("none");
    state_.store(OtaState::Checking);
    active_.store(true, std::memory_order_release);
    if (xTaskCreate(task_entry, "ota_task", settings::kOtaTaskStackBytes, this, 2, nullptr) != pdPASS) {
        active_.store(false);
        fail("task_memory");
        next_check_ = now + 60000;
    }
}
void OtaManager::task_entry(void *context) {
    auto &self = *static_cast<OtaManager *>(context);
    self.success_.store(false);
    self.run();
    self.completions_.fetch_add(1, std::memory_order_release);
    self.active_.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}
void OtaManager::run() {
    const auto started = uptime();
    auto alive = [&]() { return healthy_request() && uptime() - started < settings::kOtaOperationTimeoutMs; };
    char device[18];
    if (!device_.format(device)) {
        fail("device_identity");
        return;
    }
    auto json = std::unique_ptr<char[]>(new (std::nothrow) char[kManifestLimit + 1]);
    if (!json) {
        fail("manifest_memory");
        return;
    }
    OtaManifest manifest;
    HttpUrl endpoint;
    if (!endpoint.parse(work_config_.manifest_url.data()) || (!endpoint.tls && !signature_.available())) {
        fail("signature_key_missing");
        return;
    }
    {
        Http http;
        if (!http.open(work_config_.manifest_url.data(), device)) {
            fail("manifest_http");
            return;
        }
        const auto declared = esp_http_client_get_content_length(http.value);
        if (declared > static_cast<int64_t>(kManifestLimit)) {
            fail("manifest_size");
            return;
        }
        std::size_t used = 0;
        for (;;) {
            if (!alive()) {
                fail("manifest_cancel_or_timeout");
                return;
            }
            const int count =
                esp_http_client_read(http.value, json.get() + used, static_cast<int>(kManifestLimit + 1 - used));
            if (count < 0) {
                fail("manifest_read");
                return;
            }
            if (count == 0)
                break;
            used += static_cast<unsigned>(count);
            if (used > kManifestLimit) {
                fail("manifest_size");
                return;
            }
            vTaskDelay(1);
        }
        const char *manifest_error = "manifest_incomplete";
        if (!esp_http_client_is_complete_data_received(http.value) ||
            !read_update_manifest({json.get(), used}, !endpoint.tls, signature_, manifest, manifest_error)) {
            fail(manifest_error);
            return;
        }
    }
    json.reset();
    const auto *target = esp_ota_get_next_update_partition(nullptr);
    const auto *running = esp_ota_get_running_partition();
    if (!target || !running || target->address == running->address) {
        fail("ota_partition");
        return;
    }
    const auto decision = check_update(manifest, work_config_, device_, kFirmwareVersion, kHardwareRevision,
                                       target->size, work_attempt_.blocked ? &work_attempt_.hash : nullptr);
    const bool blocked_version =
        work_attempt_.blocked && std::strcmp(manifest.version, work_attempt_.version) == 0;
    if (decision != UpdateDecision::Install || blocked_version) {
        const char *reason = blocked_version ? "blocked_release" : update_decision_name(decision);
        if (decision != UpdateDecision::SameOrOlder || blocked_version)
            error_.store(reason);
        ESP_LOGI("ota", "No eligible release: %s", reason);
        success_.store(true);
        state_.store(OtaState::Idle);
        return;
    }
    if (!alive()) {
        fail("cancelled");
        return;
    }
    state_.store(OtaState::Downloading, std::memory_order_release);
    Download download;
    Hash hash;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_setup(&hash.value, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        fail("sha_setup");
        return;
    }
    {
        Http http;
        if (!http.open(manifest.url, device)) {
            fail("image_http");
            return;
        }
        if (esp_http_client_get_content_length(http.value) != manifest.size) {
            fail("image_size");
            return;
        }
        if (esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &download.handle) != ESP_OK) {
            fail("ota_begin");
            return;
        }
        download.open = true;
        OtaImageStream stream(manifest);
        ImageWriter writer(download.handle);
        std::array<uint8_t, 2048> buffer{};
        while (stream.total() < manifest.size) {
            if (!alive()) {
                fail("download_cancel_or_timeout");
                return;
            }
            const int count = esp_http_client_read(
                http.value, reinterpret_cast<char *>(buffer.data()),
                static_cast<int>(std::min<std::size_t>(buffer.size(), manifest.size - stream.total())));
            if (count <= 0) {
                fail("image_read");
                return;
            }
            if (psa_hash_update(&hash.value, buffer.data(), count) != PSA_SUCCESS) {
                fail("sha_update");
                return;
            }
            if (!stream.accept(buffer.data(), static_cast<std::size_t>(count), writer)) {
                fail(stream.error());
                return;
            }
            progress_.store(static_cast<unsigned>(uint64_t(stream.total()) * 100 / manifest.size));
            vTaskDelay(1);
        }
        if (!stream.complete() || !esp_http_client_is_complete_data_received(http.value)) {
            fail("image_incomplete");
            return;
        }
    }
    state_.store(OtaState::Verifying, std::memory_order_release);
    std::array<uint8_t, 32> digest{};
    std::size_t digest_size = 0;
    if (psa_hash_finish(&hash.value, digest.data(), digest.size(), &digest_size) != PSA_SUCCESS ||
        digest_size != 32 || digest != manifest.sha256) {
        fail("sha_mismatch");
        return;
    }
    const auto end_result = esp_ota_end(download.handle);
    download.open = false;
    if (end_result != ESP_OK) {
        fail("image_validation");
        return;
    }
    if (!alive()) {
        fail("cancelled");
        return;
    }
    state_.store(OtaState::AwaitingRestart, std::memory_order_release);
    const auto quiesce = uptime();
    while (!restart_allowed_.load(std::memory_order_acquire)) {
        if (!alive() || uptime() - quiesce > settings::kOtaQuiesceTimeoutMs) {
            fail("restart_not_ready");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!alive()) {
        fail("cancelled");
        return;
    }
    OtaAttempt next;
    next.pending = true;
    next.hash = manifest.sha256;
    std::memcpy(next.version, manifest.version, sizeof(next.version));
    if (!attempts_.save(next)) {
        fail("attempt_save");
        return;
    }
    if (esp_ota_set_boot_partition(target) != ESP_OK) {
        if (!attempts_.save(work_attempt_))
            ESP_LOGE("ota", "Could not restore attempt record");
        fail("boot_selection");
        return;
    }
    success_.store(true);
    state_.store(OtaState::ReadyToReboot, std::memory_order_release);
    ESP_LOGI("ota", "Verified version %s ready for restart", manifest.version);
}
} // namespace goldrop
