#pragma once

#include "goldrop/measurement.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace goldrop::sensor_protocol {
using ShtFrame = std::array<uint8_t, 6>;
using DsRom = std::array<uint8_t, 8>;
using DsScratchpad = std::array<uint8_t, 9>;

// Scratchpad-only markers: detect power loss during conversion without writing EEPROM.
inline constexpr uint8_t kDsAlarmHighMarker = 0xA5;
inline constexpr uint8_t kDsAlarmLowMarker = 0x5A;
inline constexpr uint8_t kDs12BitConfig = 0x7F;

uint8_t sht_crc(const uint8_t* bytes, std::size_t length);
uint8_t ds_crc(const uint8_t* bytes, std::size_t length);
SensorError decode_sht(const ShtFrame& frame, float& temperature, float& humidity);
SensorError validate_ds_rom(const DsRom& rom);
SensorError decode_ds(const DsScratchpad& frame, float& temperature);
}  // namespace goldrop::sensor_protocol
