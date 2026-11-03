#pragma once
#include "goldrop/measurement.hpp"
#include <cstddef>

namespace goldrop {
enum class AppendResult { Stored, DroppedOldest, Failed };

class MeasurementStore {
public:
    virtual ~MeasurementStore() = default;
    virtual AppendResult append(const Measurement& measurement) = 0;
    virtual bool peek(Measurement& measurement) const = 0;
    virtual bool acknowledge(const Measurement& measurement) = 0;
    virtual std::size_t size() const = 0;
    virtual bool persistent() const = 0;
};
}  // namespace goldrop
