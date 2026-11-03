#include "goldrop/sensor_service.hpp"
#include <cmath>

namespace goldrop {
Measurement SensorService::sample() {
    const SensorReadings reading = source_.read();
    Measurement result{};
    result.sequence = next_sequence_++;
    result.boot_id = boot_id_;
    result.device_id = device_id_;
    result.uptime_ms = clock_.uptime_ms();
    result.timestamp_valid = clock_.utc_ms(result.timestamp_ms);
    if (!result.timestamp_valid)
        result.timestamp_ms = 0;
    result.sht45_error = reading.sht45_error;
    result.ds18b20_error = reading.ds18b20_error;
    if (result.sht45_error == SensorError::None &&
        (!std::isfinite(reading.temperature_sht_c) || !std::isfinite(reading.humidity_rh) ||
         reading.temperature_sht_c < -40 || reading.temperature_sht_c > 125 || reading.humidity_rh < 0 ||
         reading.humidity_rh > 100)) {
        result.sht45_error = SensorError::InvalidValue;
    }
    if (result.ds18b20_error == SensorError::None &&
        (!std::isfinite(reading.temperature_ds18_c) || reading.temperature_ds18_c < -55 ||
         reading.temperature_ds18_c > 125)) {
        result.ds18b20_error = SensorError::InvalidValue;
    }
    result.sht45_valid = result.sht45_error == SensorError::None;
    result.ds18b20_valid = result.ds18b20_error == SensorError::None;
    if (result.sht45_valid) {
        result.temperature_sht_c = reading.temperature_sht_c;
        result.humidity_rh = reading.humidity_rh;
    }
    if (result.ds18b20_valid)
        result.temperature_ds18_c = reading.temperature_ds18_c;
    return result;
}
} // namespace goldrop
