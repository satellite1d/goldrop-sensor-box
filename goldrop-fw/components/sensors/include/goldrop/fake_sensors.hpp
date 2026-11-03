#pragma once
#include "goldrop/sensor_source.hpp"

namespace goldrop {
class FakeSensors final : public SensorSource {
public:
    explicit FakeSensors(uint32_t failure_every = 0) : failure_every_(failure_every) {}
    SensorReadings read() override;
private:
    uint32_t failure_every_{};
    uint32_t sample_count_{};
};
}  // namespace goldrop
