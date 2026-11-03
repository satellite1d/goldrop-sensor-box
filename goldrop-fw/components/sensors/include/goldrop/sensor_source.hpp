#pragma once
#include "goldrop/measurement.hpp"

namespace goldrop {
class SensorSource {
public:
    virtual ~SensorSource() = default;
    // Adapters must bound hardware waits and map protocol errors to SensorError.
    virtual SensorReadings read() = 0;
};
}  // namespace goldrop
