#pragma once
#include "goldrop/clock.hpp"
#include "goldrop/sensor_source.hpp"

namespace goldrop {
class SensorService {
public:
    SensorService(SensorSource& source, Clock& clock, uint64_t boot_id, DeviceId device_id = {})
        : source_(source), clock_(clock), boot_id_(boot_id), device_id_(device_id) {}
    Measurement sample();
private:
    SensorSource& source_;
    Clock& clock_;
    uint64_t boot_id_;
    const DeviceId device_id_;
    uint64_t next_sequence_{1};
};
}  // namespace goldrop
