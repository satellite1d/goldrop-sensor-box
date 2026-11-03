#pragma once
#include "goldrop/measurement.hpp"
#include <cstddef>

namespace goldrop {
// Returns false for invalid arguments or truncation; no allocation.
bool serialize_measurement(const Measurement& measurement, char* output, std::size_t capacity);
}  // namespace goldrop
