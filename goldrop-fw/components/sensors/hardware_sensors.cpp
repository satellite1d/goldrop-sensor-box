#include "goldrop/hardware_sensors.hpp"
#include "goldrop/settings.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace goldrop {
namespace {
void yield_until(int64_t deadline_us) {
    const int64_t remaining_us = deadline_us - esp_timer_get_time();
    if (remaining_us > 0) {
        // One extra tick accounts for the current tick phase; never read too early.
        const uint32_t remaining_ms = static_cast<uint32_t>((remaining_us + 999) / 1'000);
        vTaskDelay(pdMS_TO_TICKS(remaining_ms) + 1);
    }
}

void log_transition(const char* tag, SensorError current, SensorError previous, bool first) {
    if (!first && current == previous) return;
    if (current == SensorError::None) ESP_LOGI(tag, "Sensor detected; measurement valid");
    else if (current == SensorError::Disconnected) ESP_LOGW(tag, "Sensor disconnected / not present");
    else ESP_LOGW(tag, "Sensor error: %s", sensor_error_name(current));
}
}  // namespace

SensorReadings HardwareSensors::read() {
    SensorReadings result{};
    result.sht45_error = sht_.begin_conversion();
    const int64_t sht_deadline = esp_timer_get_time() + settings::kShtConversionMs * 1'000LL;
    result.ds18b20_error = onewire_.initialize();
    if (result.ds18b20_error == SensorError::None) result.ds18b20_error = ds_.begin_conversion();
    const int64_t ds_deadline = esp_timer_get_time() + settings::kDsConversionMs * 1'000LL;
    if (result.sht45_error == SensorError::None) {
        yield_until(sht_deadline);
        result.sht45_error = sht_.read_measurement(result.temperature_sht_c, result.humidity_rh);
    }
    if (result.ds18b20_error == SensorError::None) {
        yield_until(ds_deadline);
        result.ds18b20_error = ds_.read_temperature(result.temperature_ds18_c);
    }
    log_status(result);
    return result;
}

void HardwareSensors::log_status(const SensorReadings& readings) {
    log_transition("sht45", readings.sht45_error, previous_sht_, !status_logged_);
    log_transition("ds18b20", readings.ds18b20_error, previous_ds_, !status_logged_);
    previous_sht_ = readings.sht45_error;
    previous_ds_ = readings.ds18b20_error;
    status_logged_ = true;
}
}  // namespace goldrop
