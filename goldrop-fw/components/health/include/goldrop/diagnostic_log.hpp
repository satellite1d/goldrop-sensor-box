#pragma once

#include <cstddef>
#include <cstdint>
#include "goldrop/settings.hpp"

namespace goldrop {
struct Measurement;
struct Health;
struct NetworkStatus;

void log_measurement(const Measurement &measurement, bool buffered);
void log_health(const Health &health, std::size_t buffered, int64_t uptime_ms, uint32_t free_heap,
                const NetworkStatus &network, std::size_t capacity = settings::kRamHistoryCapacity,
                bool persistent = false);
} // namespace goldrop
