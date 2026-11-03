#pragma once

#include "goldrop/device_id.hpp"
#include <cstdint>
#include <type_traits>

namespace goldrop {
enum class SensorError : uint8_t {
    None, Disconnected, Timeout, Crc, InvalidValue, NotInitialized, Communication
};

inline const char* sensor_error_name(SensorError error) {
    switch (error) {
        case SensorError::None: return "none";
        case SensorError::Disconnected: return "disconnected";
        case SensorError::Timeout: return "timeout";
        case SensorError::Crc: return "crc";
        case SensorError::InvalidValue: return "invalid_value";
        case SensorError::NotInitialized: return "not_initialized";
        case SensorError::Communication: return "communication";
    }
    return "unknown";
}

struct SensorReadings {
    float temperature_sht_c{};
    float humidity_rh{};
    float temperature_ds18_c{};
    SensorError sht45_error{SensorError::NotInitialized};
    SensorError ds18b20_error{SensorError::NotInitialized};
};

struct Measurement {
    uint64_t sequence{};
    uint64_t boot_id{};
    int64_t uptime_ms{};
    int64_t timestamp_ms{};
    float temperature_sht_c{};
    float humidity_rh{};
    float temperature_ds18_c{};
    bool timestamp_valid{};
    bool sht45_valid{};
    bool ds18b20_valid{};
    SensorError sht45_error{SensorError::NotInitialized};
    SensorError ds18b20_error{SensorError::NotInitialized};
    DeviceId device_id;
};
static_assert(std::is_trivially_copyable_v<Measurement>,
              "FreeRTOS queues copy measurements as bytes");

inline bool same_measurement(const Measurement& a, const Measurement& b) {
    return a.device_id == b.device_id && a.boot_id == b.boot_id && a.sequence == b.sequence;
}
}  // namespace goldrop
