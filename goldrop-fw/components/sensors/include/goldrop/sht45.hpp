#pragma once

#include "goldrop/measurement.hpp"
#include "driver/i2c_master.h"

namespace goldrop {
class Sht45 {
public:
    Sht45() = default;
    ~Sht45();
    Sht45(const Sht45&) = delete;
    Sht45& operator=(const Sht45&) = delete;
    SensorError begin_conversion();
    // Caller yields for settings::kShtConversionMs before reading.
    SensorError read_measurement(float& temperature, float& humidity);
private:
    esp_err_t initialize();
    SensorError transaction_result(esp_err_t result);
    i2c_master_bus_handle_t bus_{};
    i2c_master_dev_handle_t device_{};
    bool converting_{};
};
}  // namespace goldrop
