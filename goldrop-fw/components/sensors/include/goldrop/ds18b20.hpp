#pragma once

#include "goldrop/sensor_protocol.hpp"

namespace goldrop {
class OneWireBus {
public:
    virtual ~OneWireBus() = default;
    virtual SensorError reset() = 0;
    virtual SensorError write(const uint8_t* data, std::size_t size) = 0;
    virtual SensorError read(uint8_t* data, std::size_t size) = 0;
    virtual SensorError read_bit(uint8_t& bit) = 0;
};

// One external, normally powered DS18B20 per connector. No heap allocations.
class Ds18b20 {
public:
    explicit Ds18b20(OneWireBus& bus) : bus_(bus) {}
    SensorError begin_conversion();
    // Caller must yield for settings::kDsConversionMs after begin_conversion succeeds.
    SensorError read_temperature(float& temperature);
private:
    SensorError read_rom(sensor_protocol::DsRom& rom);
    SensorError command(uint8_t command);
    OneWireBus& bus_;
    sensor_protocol::DsRom rom_{};
    bool converting_{};
};
}  // namespace goldrop
