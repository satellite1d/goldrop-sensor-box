#pragma once

#include "goldrop/esp_onewire_bus.hpp"
#include "goldrop/sensor_source.hpp"
#include "goldrop/sht45.hpp"

namespace goldrop {
class HardwareSensors final : public SensorSource {
public:
    HardwareSensors() : ds_(onewire_) {}
    SensorReadings read() override;
private:
    void log_status(const SensorReadings& readings);
    Sht45 sht_;
    EspOneWireBus onewire_;
    Ds18b20 ds_;
    SensorError previous_sht_{SensorError::NotInitialized};
    SensorError previous_ds_{SensorError::NotInitialized};
    bool status_logged_{};
};
}  // namespace goldrop
