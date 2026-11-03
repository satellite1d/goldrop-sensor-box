#pragma once
#include "goldrop/measurement_store.hpp"
#include <array>

namespace goldrop {
// Single owner: telemetry_task. Cross-task acquisition uses a FreeRTOS queue.
template <std::size_t Capacity>
class RamMeasurementStore final : public MeasurementStore {
    static_assert(Capacity > 0);
public:
    AppendResult append(const Measurement& measurement) override {
        const bool full = count_ == Capacity;
        if (full) {
            head_ = (head_ + 1) % Capacity;
            --count_;
        }
        records_[(head_ + count_) % Capacity] = measurement;
        ++count_;
        return full ? AppendResult::DroppedOldest : AppendResult::Stored;
    }
    bool peek(Measurement& measurement) const override {
        if (count_ == 0) return false;
        measurement = records_[head_];
        return true;
    }
    bool acknowledge(const Measurement& measurement) override {
        if (count_ == 0 || !same_measurement(records_[head_], measurement)) return false;
        head_ = (head_ + 1) % Capacity;
        --count_;
        return true;
    }
    std::size_t size() const override { return count_; }
    bool persistent() const override { return false; }
private:
    std::array<Measurement, Capacity> records_{};
    std::size_t head_{};
    std::size_t count_{};
};
}  // namespace goldrop
