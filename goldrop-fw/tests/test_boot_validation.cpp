#include "goldrop/boot_validation.hpp"
#include "goldrop/settings.hpp"
#include "esp_ota_ops.h"
#include <cstdio>
#include <cstdlib>
namespace {
esp_partition_t partition{0x10000};
esp_ota_img_states_t state = ESP_OTA_IMG_PENDING_VERIFY;
unsigned confirmations{}, rollbacks{}, failures{};
esp_err_t state_result = ESP_OK;
void require(bool ok, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL boot validation line %d\n", line);
        std::exit(1);
    }
}
#define REQUIRE(...) require((__VA_ARGS__), __LINE__)
} // namespace
const esp_partition_t *esp_ota_get_running_partition() {
    return &partition;
}
esp_err_t esp_ota_get_state_partition(const esp_partition_t *, esp_ota_img_states_t *value) {
    *value = state;
    return state_result;
}
esp_err_t esp_ota_mark_app_valid_cancel_rollback() {
    ++confirmations;
    if (failures) {
        --failures;
        return ESP_FAIL;
    }
    return ESP_OK;
}
esp_err_t esp_ota_mark_app_invalid_rollback_and_reboot() {
    ++rollbacks;
    return ESP_FAIL;
}
int main() {
    using namespace goldrop;
    BootValidation normal;
    REQUIRE(normal.begin() == ESP_OK && !normal.ready());
    normal.poll(true, 1, 1, 1000);
    REQUIRE(confirmations == 0);
    normal.poll(true, 2, 2, settings::kBootObservationMs);
    REQUIRE(confirmations == 1 && normal.ready());
    confirmations = 0;
    failures = 3;
    BootValidation retry;
    REQUIRE(retry.begin() == ESP_OK);
    retry.poll(true, 2, 2, 20000);
    retry.poll(true, 2, 2, 20100);
    REQUIRE(confirmations == 1 && !retry.ready());
    retry.poll(true, 3, 3, 22000);
    retry.poll(true, 3, 3, 24000);
    REQUIRE(confirmations == 3);
    retry.poll(true, 3, 3, 26000);
    REQUIRE(rollbacks == 1 && !retry.ready());
    retry.poll(true, 3, 3, 26001);
    REQUIRE(rollbacks == 1);
    retry.poll(true, 3, 3, 31000);
    REQUIRE(rollbacks == 2);
    BootValidation broken;
    REQUIRE(broken.begin() == ESP_OK);
    broken.poll(false, 0, 0, 0);
    REQUIRE(rollbacks == 3);
    state = ESP_OTA_IMG_VALID;
    BootValidation existing;
    REQUIRE(existing.begin() == ESP_OK && existing.ready());
    state = ESP_OTA_IMG_UNDEFINED;
    BootValidation initial;
    REQUIRE(initial.begin() == ESP_OK && initial.ready());
    state_result = ESP_ERR_NOT_FOUND;
    BootValidation usb;
    REQUIRE(usb.begin() == ESP_OK && usb.ready());
    state_result = ESP_FAIL;
    BootValidation unreadable;
    REQUIRE(unreadable.begin() == ESP_FAIL && unreadable.ready());
    std::puts("PASS: boot observation, confirmation retry, bounded rollback retry, ordinary valid image");
}
