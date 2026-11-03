#include "goldrop/sht45.hpp"
#include "goldrop/sensor_protocol.hpp"
#include "goldrop/settings.hpp"
#include "esp_log.h"

namespace goldrop {
namespace {
constexpr const char* kTag = "sht45";
void log_cleanup(esp_err_t result) {
    if (result != ESP_OK) ESP_LOGE(kTag, "Resource release failed: %s", esp_err_to_name(result));
}
}  // namespace

Sht45::~Sht45() {
    if (device_) log_cleanup(i2c_master_bus_rm_device(device_));
    if (bus_) log_cleanup(i2c_del_master_bus(bus_));
}

esp_err_t Sht45::initialize() {
    if (!bus_) {
        i2c_master_bus_config_t config{};
        config.i2c_port = I2C_NUM_0;
        config.sda_io_num = static_cast<gpio_num_t>(settings::kShtSdaGpio);
        config.scl_io_num = static_cast<gpio_num_t>(settings::kShtSclGpio);
        config.clk_source = I2C_CLK_SRC_DEFAULT;
        config.glitch_ignore_cnt = 7;
        // Goldrop has external pull-ups; the weak internal pull-ups stay disabled.
        const esp_err_t result = i2c_new_master_bus(&config, &bus_);
        if (result != ESP_OK) return result;
    }
    if (!device_) {
        i2c_device_config_t config{};
        config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
        config.device_address = settings::kShtI2cAddress;
        config.scl_speed_hz = settings::kShtI2cSpeedHz;
        const esp_err_t result = i2c_master_bus_add_device(bus_, &config, &device_);
        if (result != ESP_OK) return result;
        ESP_LOGI(kTag, "I2C SDA=%d SCL=%d address=0x%02x", settings::kShtSdaGpio,
                 settings::kShtSclGpio, settings::kShtI2cAddress);
    }
    return ESP_OK;
}

SensorError Sht45::transaction_result(esp_err_t result) {
    if (result == ESP_OK) return SensorError::None;
    if (result == ESP_ERR_NOT_FOUND || result == ESP_ERR_INVALID_RESPONSE) {
        return SensorError::Disconnected;
    }
    if (result == ESP_ERR_TIMEOUT) {
        const esp_err_t recovery = i2c_master_bus_reset(bus_);
        if (recovery != ESP_OK) {
            ESP_LOGW(kTag, "I2C bus recovery failed: %s", esp_err_to_name(recovery));
        }
        return SensorError::Timeout;
    }
    ESP_LOGW(kTag, "I2C operation failed: %s", esp_err_to_name(result));
    return SensorError::Communication;
}

SensorError Sht45::begin_conversion() {
    converting_ = false;
    const esp_err_t initialized = initialize();
    if (initialized != ESP_OK) {
        ESP_LOGW(kTag, "I2C initialization failed: %s", esp_err_to_name(initialized));
        return SensorError::NotInitialized;
    }
    SensorError result = transaction_result(
        i2c_master_probe(bus_, settings::kShtI2cAddress, settings::kI2cTimeoutMs));
    if (result != SensorError::None) return result;
    constexpr uint8_t command = 0xFD;  // High precision, heater off.
    result = transaction_result(i2c_master_transmit(device_, &command, 1, settings::kI2cTimeoutMs));
    converting_ = result == SensorError::None;
    return result;
}

SensorError Sht45::read_measurement(float& temperature, float& humidity) {
    if (!converting_) return SensorError::NotInitialized;
    converting_ = false;
    sensor_protocol::ShtFrame frame{};
    const SensorError result = transaction_result(
        i2c_master_receive(device_, frame.data(), frame.size(), settings::kI2cTimeoutMs));
    if (result != SensorError::None) return result;
    return sensor_protocol::decode_sht(frame, temperature, humidity);
}
}  // namespace goldrop
