#include "goldrop/boot_validation.hpp"
#include "goldrop/boot_self_test.hpp"
#include "esp_log.h"
#include "esp_ota_ops.h"

namespace goldrop {
esp_err_t BootValidation::begin() {
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == nullptr)
        return ESP_ERR_NOT_FOUND;
    esp_ota_img_states_t state{};
    const esp_err_t result = esp_ota_get_state_partition(running, &state);
    // Initial USB installation has no recorded OTA state yet.
    if (result == ESP_ERR_NOT_FOUND)
        return ESP_OK;
    if (result != ESP_OK)
        return result;
    pending_ = state == ESP_OTA_IMG_PENDING_VERIFY;
    if (pending_)
        ESP_LOGI("ota", "New image awaits local boot validation");
    return ESP_OK;
}

void BootValidation::poll(bool core_initialized, uint32_t acquired, uint32_t buffered, int64_t elapsed_ms) {
    if (!pending_)
        return;
    BootDecision decision = boot_self_test(core_initialized, acquired, buffered, elapsed_ms);
    if (confirm_attempts_ >= 3 || elapsed_ms >= settings::kBootValidationDeadlineMs)
        decision = BootDecision::Rollback;
    if (decision == BootDecision::Wait)
        return;
    if (decision == BootDecision::Confirm) {
        if (elapsed_ms < next_attempt_ms_)
            return;
        ++confirm_attempts_;
        next_attempt_ms_ = elapsed_ms + 2'000;
        const esp_err_t result = esp_ota_mark_app_valid_cancel_rollback();
        if (result == ESP_OK) {
            pending_ = false;
            ESP_LOGI("ota", "Image confirmed after local self-test");
        } else
            ESP_LOGE("ota", "Could not confirm image: %s; image remains unconfirmed",
                     esp_err_to_name(result));
    } else {
        if (elapsed_ms < next_attempt_ms_)
            return;
        next_attempt_ms_ = elapsed_ms + 5'000;
        ESP_LOGE("ota", "Local boot self-test failed; requesting rollback");
        const esp_err_t result = esp_ota_mark_app_invalid_rollback_and_reboot();
        // Success reboots and never returns; failure must be reported without a restart loop.
        ESP_LOGE("ota", "Rollback did not reboot: %s", esp_err_to_name(result));
    }
}
} // namespace goldrop
