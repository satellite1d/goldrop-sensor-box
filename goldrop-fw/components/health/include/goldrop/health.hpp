#pragma once
#include <atomic>
#include <cstdint>

namespace goldrop {
struct Health {
    std::atomic<uint32_t> acquired{0};
    std::atomic<uint32_t> acquisition_drops{0};
    std::atomic<uint32_t> sensor_failures{0};
    // Remaining fields are owned by telemetry_task.
    uint32_t buffered{};
    uint32_t history_drops{};
};
}  // namespace goldrop
