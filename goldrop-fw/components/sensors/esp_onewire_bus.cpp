#include "goldrop/esp_onewire_bus.hpp"
#include "goldrop/settings.hpp"
#include "esp_log.h"
#include <limits>

namespace goldrop {
namespace {
constexpr const char* kTag = "onewire";
SensorError bus_result(esp_err_t result) {
    if (result == ESP_OK) return SensorError::None;
    if (result == ESP_ERR_NOT_FOUND) return SensorError::Disconnected;
    if (result == ESP_ERR_TIMEOUT) return SensorError::Timeout;
    if (result == ESP_ERR_INVALID_CRC) return SensorError::Crc;
    return SensorError::Communication;
}
}  // namespace

EspOneWireBus::~EspOneWireBus() {
    if (bus_) {
        const esp_err_t result = onewire_bus_del(bus_);
        if (result != ESP_OK) ESP_LOGE(kTag, "Resource release failed: %s", esp_err_to_name(result));
    }
}

SensorError EspOneWireBus::initialize() {
    if (bus_) return SensorError::None;
    onewire_bus_config_t config{};
    config.bus_gpio_num = settings::kDsDataGpio;
    // External 4.7 kOhm pull-up on Goldrop, normal 3.3 V sensor power.
    onewire_bus_rmt_config_t rmt{};
    rmt.max_rx_bytes = sensor_protocol::DsScratchpad{}.size();
    const esp_err_t result = onewire_new_bus_rmt(&config, &rmt, &bus_);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "RMT initialization failed: %s", esp_err_to_name(result));
        return SensorError::NotInitialized;
    }
    ESP_LOGI(kTag, "1-Wire RMT GPIO=%d; one externally powered DS18B20", settings::kDsDataGpio);
    return SensorError::None;
}

SensorError EspOneWireBus::reset() {
    return bus_ ? operation_result(onewire_bus_reset(bus_)) : SensorError::NotInitialized;
}
SensorError EspOneWireBus::write(const uint8_t* data, std::size_t size) {
    if (!bus_) return SensorError::NotInitialized;
    if (size > std::numeric_limits<uint8_t>::max()) return SensorError::InvalidValue;
    return operation_result(onewire_bus_write_bytes(bus_, data, static_cast<uint8_t>(size)));
}
SensorError EspOneWireBus::read(uint8_t* data, std::size_t size) {
    return bus_ ? operation_result(onewire_bus_read_bytes(bus_, data, size)) : SensorError::NotInitialized;
}
SensorError EspOneWireBus::read_bit(uint8_t& bit) {
    return bus_ ? operation_result(onewire_bus_read_bit(bus_, &bit)) : SensorError::NotInitialized;
}

SensorError EspOneWireBus::operation_result(esp_err_t result) {
    const SensorError error = bus_result(result);
    if (error == SensorError::Timeout || error == SensorError::Communication) {
        // Cancel pending RMT operations and discard stale receive events after a failure.
        // Only sensor_task owns this bus. Reinitialize once on the next sampling cycle.
        ESP_LOGW(kTag, "Bus failure: %s; reinitializing on next sample", esp_err_to_name(result));
        const esp_err_t released = onewire_bus_del(bus_);
        if (released == ESP_OK) bus_ = nullptr;
        else ESP_LOGE(kTag, "Bus recovery release failed: %s", esp_err_to_name(released));
    }
    return error;
}
}  // namespace goldrop
