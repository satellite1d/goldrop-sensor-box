#include "goldrop/sensor_protocol.hpp"
#include <algorithm>

namespace goldrop::sensor_protocol {
uint8_t sht_crc(const uint8_t* bytes, std::size_t length) {
    uint8_t crc = 0xFF;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = static_cast<uint8_t>((crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1);
        }
    }
    return crc;
}

uint8_t ds_crc(const uint8_t* bytes, std::size_t length) {
    uint8_t crc = 0;
    for (std::size_t i = 0; i < length; ++i) {
        crc ^= bytes[i];
        for (unsigned bit = 0; bit < 8; ++bit) {
            crc = static_cast<uint8_t>((crc & 1) ? (crc >> 1) ^ 0x8C : crc >> 1);
        }
    }
    return crc;
}

SensorError decode_sht(const ShtFrame& frame, float& temperature, float& humidity) {
    if (sht_crc(frame.data(), 2) != frame[2] || sht_crc(frame.data() + 3, 2) != frame[5]) {
        return SensorError::Crc;
    }
    const uint16_t raw_t = (static_cast<uint16_t>(frame[0]) << 8) | frame[1];
    const uint16_t raw_rh = (static_cast<uint16_t>(frame[3]) << 8) | frame[4];
    const float t = -45.0F + 175.0F * raw_t / 65'535.0F;
    // SHT4x permits raw RH slightly beyond 0..100%; clip as specified by Sensirion.
    const float rh = std::clamp(-6.0F + 125.0F * raw_rh / 65'535.0F, 0.0F, 100.0F);
    if (t < -40.0F || t > 125.0F) return SensorError::InvalidValue;
    temperature = t;
    humidity = rh;
    return SensorError::None;
}

SensorError validate_ds_rom(const DsRom& rom) {
    if (ds_crc(rom.data(), 7) != rom[7]) return SensorError::Crc;
    return rom[0] == 0x28 ? SensorError::None : SensorError::InvalidValue;
}

SensorError decode_ds(const DsScratchpad& frame, float& temperature) {
    if (ds_crc(frame.data(), 8) != frame[8]) return SensorError::Crc;
    if ((frame[4] & 0x9F) != 0x1F) return SensorError::InvalidValue;
    const unsigned resolution = (frame[4] >> 5) & 3;
    uint16_t raw = (static_cast<uint16_t>(frame[1]) << 8) | frame[0];
    raw &= static_cast<uint16_t>(~((1U << (3 - resolution)) - 1));
    const int32_t signed_raw = raw < 0x8000 ? raw : static_cast<int32_t>(raw) - 65'536;
    const float t = signed_raw / 16.0F;
    if (t < -55.0F || t > 125.0F) return SensorError::InvalidValue;
    temperature = t;
    return SensorError::None;
}
}  // namespace goldrop::sensor_protocol
