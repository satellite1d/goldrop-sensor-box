#pragma once

#include "goldrop/ds18b20.hpp"
#include "onewire_bus.h"

namespace goldrop {
class EspOneWireBus final : public OneWireBus {
public:
    EspOneWireBus() = default;
    ~EspOneWireBus() override;
    EspOneWireBus(const EspOneWireBus&) = delete;
    EspOneWireBus& operator=(const EspOneWireBus&) = delete;
    SensorError initialize();
    SensorError reset() override;
    SensorError write(const uint8_t* data, std::size_t size) override;
    SensorError read(uint8_t* data, std::size_t size) override;
    SensorError read_bit(uint8_t& bit) override;
private:
    SensorError operation_result(esp_err_t result);
    onewire_bus_handle_t bus_{};
};
}  // namespace goldrop
