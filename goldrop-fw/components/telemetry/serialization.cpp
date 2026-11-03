#include "goldrop/serialization.hpp"
#include "goldrop/utc_timestamp.hpp"
#include <cmath>
#include <cinttypes>
#include <cstdio>

namespace goldrop {
namespace {
void format_value(float value, bool valid, char (&output)[48]) {
    if (valid && std::isfinite(value))
        std::snprintf(output, sizeof(output), "%.3f", static_cast<double>(value));
    else
        std::snprintf(output, sizeof(output), "null");
}
} // namespace

bool serialize_measurement(const Measurement &m, char *output, std::size_t capacity) {
    if (output == nullptr || capacity == 0)
        return false;
    char sht[48], rh[48], ds[48], timestamp[32];
    char utc[kUtcTimestampCapacity], timestamp_utc[kUtcTimestampCapacity + 2] = "null";
    char mac[18], device_id[20] = "null";
    if (m.device_id.format(mac))
        std::snprintf(device_id, sizeof(device_id), "\"%s\"", mac);
    const bool sht_valid = m.sht45_valid && m.sht45_error == SensorError::None &&
                           std::isfinite(m.temperature_sht_c) && std::isfinite(m.humidity_rh) &&
                           m.temperature_sht_c >= -40 && m.temperature_sht_c <= 125 && m.humidity_rh >= 0 &&
                           m.humidity_rh <= 100;
    const bool ds_valid = m.ds18b20_valid && m.ds18b20_error == SensorError::None &&
                          std::isfinite(m.temperature_ds18_c) && m.temperature_ds18_c >= -55 &&
                          m.temperature_ds18_c <= 125;
    const auto sht_error =
        !sht_valid && m.sht45_error == SensorError::None ? SensorError::InvalidValue : m.sht45_error;
    const auto ds_error =
        !ds_valid && m.ds18b20_error == SensorError::None ? SensorError::InvalidValue : m.ds18b20_error;
    format_value(m.temperature_sht_c, sht_valid, sht);
    format_value(m.humidity_rh, sht_valid, rh);
    format_value(m.temperature_ds18_c, ds_valid, ds);
    if (m.timestamp_valid && format_utc_timestamp(m.timestamp_ms, utc, sizeof(utc))) {
        std::snprintf(timestamp, sizeof(timestamp), "%" PRId64, m.timestamp_ms);
        std::snprintf(timestamp_utc, sizeof(timestamp_utc), "\"%s\"", utc);
    } else
        std::snprintf(timestamp, sizeof(timestamp), "null");
    const int length = std::snprintf(
        output, capacity,
        "{\"schema_version\":1,\"device_id\":%s,\"sequence\":\"%" PRIu64 "\",\"boot_id\":\"%" PRIu64
        "\",\"uptime_ms\":%" PRId64 ",\"timestamp_ms\":%s,\"timestamp_utc\":%s,"
        "\"temperature_sht_c\":%s,\"humidity_rh\":%s,\"temperature_ds18_c\":%s,"
        "\"sht45_valid\":%s,\"ds18b20_valid\":%s,\"sht45_error\":\"%s\",\"ds18b20_error\":\"%s\"}",
        device_id, m.sequence, m.boot_id, m.uptime_ms, timestamp, timestamp_utc, sht, rh, ds,
        sht_valid ? "true" : "false", ds_valid ? "true" : "false", sensor_error_name(sht_error),
        sensor_error_name(ds_error));
    if (length < 0 || static_cast<std::size_t>(length) >= capacity) {
        output[0] = '\0';
        return false;
    }
    return true;
}
} // namespace goldrop
