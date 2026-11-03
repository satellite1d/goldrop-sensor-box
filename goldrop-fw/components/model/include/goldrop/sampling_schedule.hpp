#pragma once
#include "goldrop/settings.hpp"
#include <algorithm>

namespace goldrop {
// Boot validation needs two acquired and persisted samples within its 45 s deadline.
// Keep the saved setting intact; only the first wait after each boot is shortened.
inline uint32_t sampling_interval_after_acquisition(uint32_t configured_ms, uint32_t acquired,
                                                    bool configuration_trial = false) {
    return (acquired < 2 || configuration_trial) ? std::min(configured_ms, settings::kSamplingIntervalMs)
                                                 : configured_ms;
}
} // namespace goldrop
