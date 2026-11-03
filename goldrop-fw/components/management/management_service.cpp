#include "goldrop/management_service.hpp"
#include "goldrop/management_crypto.hpp"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
namespace goldrop {
void ManagementService::poll(int64_t now, bool network, bool time, bool allowed) {
    if (active_) {
        if (!done_.load(std::memory_order_acquire))
            return;
        active_ = false;
        const auto &current = config_.config_state();
        const bool unchanged = current.report_sequence == snapshot_.report_sequence &&
                               current.management.endpoint == snapshot_.management.endpoint &&
                               current.management.key == snapshot_.management.key &&
                               current.management.enabled;
        if (success_ && unchanged && allowed && config_.can_poll(now)) {
            failures_ = 0;
            if (reply_.action != ConfigAction::None)
                config_.accept(reply_, now);
            next_ = now + (config_.config_state().phase == ConfigPhase::Testing ? 2'000 : kConfigPollMs) +
                    esp_random() % 5001;
        } else {
            failures_ = std::min(failures_ + 1, 4U);
            next_ = now +
                    std::min<int64_t>(5'000LL << (failures_ - 1),
                                      config_.config_state().phase == ConfigPhase::Testing ? 10'000 : 60'000);
            if (!success_)
                ESP_LOGW("management", "Exchange failed; configuration unchanged; retry in background");
        }
        snapshot_.clear();
        reply_.clear();
        clear_secret(challenge_.data(), challenge_.size());
    }
    const auto &state = config_.config_state();
    if (!allowed || !network || !id_.valid() || !config_.can_poll(now) || !state.management.enrolled ||
        !state.management.enabled)
        return;
    HttpUrl url;
    if (!url.parse(state.management.endpoint.data()) || (url.tls && !time))
        return;
    const bool probe = config_.probe_ready();
    if (observed_report_ != state.report_sequence || observed_probe_ != probe) {
        observed_report_ = state.report_sequence;
        observed_probe_ = probe;
        next_ = std::min(next_, now + 1000);
    }
    if (now < next_)
        return;
    if (exchange_sequence_ == kMaxConfigRevision)
        return;
    ++exchange_sequence_;
    snapshot_ = state;
    probe_ = probe;
    esp_fill_random(challenge_.data(), challenge_.size());
    success_ = false;
    done_.store(false, std::memory_order_release);
    active_ = true;
    if (xTaskCreate(task_entry, "management_http", settings::kManagementTaskStackBytes, this, 2, nullptr) !=
        pdPASS) {
        active_ = false;
        snapshot_.clear();
        next_ = now + 60'000;
        ESP_LOGW("management", "Worker allocation failed");
    }
}
void ManagementService::task_entry(void *context) {
    auto &self = *static_cast<ManagementService *>(context);
    self.exchange();
    self.done_.store(true, std::memory_order_release);
    vTaskDelete(nullptr);
}
void ManagementService::exchange() {
    ManagementRequest plain;
    encode_management_request(snapshot_, challenge_, true, probe_, exchange_sequence_, plain);
    std::array<uint8_t, 168> request{};
    std::array<uint8_t, 544> response{};
    ManagementResponse decoded{};
    std::size_t n = 0;
    if (!management_encrypt(snapshot_.management.key, id_, 'C', plain.data(), plain.size(), request.data(),
                            request.size(), n)) {
        clear_secret(plain.data(), plain.size());
        return;
    }
    clear_secret(plain.data(), plain.size());
    esp_http_client_config_t options{};
    options.url = snapshot_.management.endpoint.data();
    options.method = HTTP_METHOD_POST;
    options.timeout_ms = settings::kManagementIoTimeoutMs;
    options.disable_auto_redirect = true;
    options.crt_bundle_attach = esp_crt_bundle_attach;
    options.buffer_size = 768;
    options.buffer_size_tx = 256;
    options.keep_alive_enable = false;
    esp_http_client_handle_t client = esp_http_client_init(&options);
    const int64_t deadline = esp_timer_get_time() + settings::kManagementOperationTimeoutUs;
    bool ok =
        client && esp_http_client_set_header(client, "Content-Type", "application/octet-stream") == ESP_OK;
    if (ok)
        ok = esp_http_client_open(client, int(n)) == ESP_OK;
    if (ok)
        ok = esp_http_client_write(client, reinterpret_cast<const char *>(request.data()), int(n)) == int(n);
    if (ok)
        ok = esp_http_client_fetch_headers(client) == int64_t(response.size()) &&
             esp_http_client_get_status_code(client) == 200;
    std::size_t received = 0;
    while (ok && received < response.size()) {
        if (esp_timer_get_time() >= deadline) {
            ok = false;
            break;
        }
        const int count = esp_http_client_read(client, reinterpret_cast<char *>(response.data() + received),
                                               int(response.size() - received));
        if (count <= 0) {
            ok = false;
            break;
        }
        received += std::size_t(count);
    }
    if (client) {
        const auto result = esp_http_client_cleanup(client);
        ok = ok && result == ESP_OK;
    }
    if (ok)
        ok = management_decrypt(snapshot_.management.key, id_, 'S', response.data(), response.size(),
                                decoded.data(), decoded.size(), n) &&
             n == decoded.size() && decode_management_response(decoded, challenge_, reply_);
    success_ = ok;
    clear_secret(request.data(), request.size());
    clear_secret(response.data(), response.size());
    clear_secret(decoded.data(), decoded.size());
}
} // namespace goldrop
