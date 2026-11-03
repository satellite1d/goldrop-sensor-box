#pragma once
#include "goldrop/settings.hpp"

namespace goldrop {
enum class BootDecision { Wait, Confirm, Rollback };

// Sensor validity and connectivity deliberately are not prerequisites.
inline BootDecision boot_self_test(bool core_initialized, uint32_t acquired, uint32_t buffered,
                                   int64_t elapsed_ms) {
    if (!core_initialized)
        return BootDecision::Rollback;
    if (elapsed_ms >= settings::kBootObservationMs && acquired >= 2 && buffered >= 2)
        return BootDecision::Confirm;
    if (elapsed_ms >= settings::kBootValidationDeadlineMs)
        return BootDecision::Rollback;
    return BootDecision::Wait;
}
} // namespace goldrop
