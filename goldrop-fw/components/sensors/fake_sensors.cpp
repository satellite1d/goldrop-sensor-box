#include "goldrop/fake_sensors.hpp"

namespace goldrop {
SensorReadings FakeSensors::read() {
    ++sample_count_;
    SensorReadings result{22.5F, 45.0F, 21.75F, SensorError::None, SensorError::None};
    if (failure_every_ != 0 && sample_count_ % failure_every_ == 0) {
        result.ds18b20_error = SensorError::Disconnected;
    }
    return result;
}
}  // namespace goldrop
