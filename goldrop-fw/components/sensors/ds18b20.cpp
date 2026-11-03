#include "goldrop/ds18b20.hpp"
#include <algorithm>

namespace goldrop {
SensorError Ds18b20::read_rom(sensor_protocol::DsRom& rom) {
    SensorError result = bus_.reset();
    if (result != SensorError::None) return result;
    constexpr uint8_t read_rom = 0x33;
    result = bus_.write(&read_rom, 1);
    if (result != SensorError::None) return result;
    result = bus_.read(rom.data(), rom.size());
    if (result != SensorError::None) return result;
    return sensor_protocol::validate_ds_rom(rom);
}

SensorError Ds18b20::command(uint8_t command_byte) {
    SensorError result = bus_.reset();
    if (result != SensorError::None) return result;
    uint8_t bytes[10]{};
    bytes[0] = 0x55;  // Match ROM: never accept a result from a replacement sensor.
    std::copy(rom_.begin(), rom_.end(), bytes + 1);
    bytes[9] = command_byte;
    return bus_.write(bytes, sizeof(bytes));
}

SensorError Ds18b20::begin_conversion() {
    converting_ = false;
    SensorError result = read_rom(rom_);
    if (result != SensorError::None) return result;
    result = command(0x4E);  // Write volatile scratchpad, never Copy Scratchpad / EEPROM.
    if (result != SensorError::None) return result;
    constexpr uint8_t config[] = {sensor_protocol::kDsAlarmHighMarker,
                                 sensor_protocol::kDsAlarmLowMarker,
                                 sensor_protocol::kDs12BitConfig};
    result = bus_.write(config, sizeof(config));
    if (result != SensorError::None) return result;
    result = command(0x44);  // Convert temperature.
    if (result != SensorError::None) return result;
    uint8_t ready = 1;
    result = bus_.read_bit(ready);
    if (result != SensorError::None) return result;
    if (ready != 0) return SensorError::InvalidValue;  // Conversion was not accepted.
    converting_ = true;
    return SensorError::None;
}

SensorError Ds18b20::read_temperature(float& temperature) {
    if (!converting_) return SensorError::NotInitialized;
    converting_ = false;
    uint8_t ready = 0;
    SensorError result = bus_.read_bit(ready);
    if (result != SensorError::None) return result;
    if (ready == 0) return SensorError::Timeout;
    sensor_protocol::DsRom current_rom{};
    result = read_rom(current_rom);
    if (result != SensorError::None) return result;
    if (current_rom != rom_) return SensorError::Disconnected;
    result = command(0xBE);  // Read scratchpad only after completed conversion.
    if (result != SensorError::None) return result;
    sensor_protocol::DsScratchpad scratchpad{};
    result = bus_.read(scratchpad.data(), scratchpad.size());
    if (result != SensorError::None) return result;
    float value = 0;
    result = sensor_protocol::decode_ds(scratchpad, value);
    if (result != SensorError::None) return result;
    if (scratchpad[2] != sensor_protocol::kDsAlarmHighMarker ||
        scratchpad[3] != sensor_protocol::kDsAlarmLowMarker ||
        scratchpad[4] != sensor_protocol::kDs12BitConfig) {
        return SensorError::InvalidValue;  // Power loss: old/power-on value, not a fresh result.
    }
    temperature = value;  // 85 C is valid if this conversion really completed.
    return SensorError::None;
}
}  // namespace goldrop
