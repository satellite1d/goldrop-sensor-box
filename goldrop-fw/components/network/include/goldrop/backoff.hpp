#pragma once
#include <cstdint>

namespace goldrop {
// Timing policy only; scheduling and jitter belong to the network adapter.
class Backoff {
public:
    Backoff(uint32_t initial_ms = 1'000, uint32_t maximum_ms = 60'000)
        : initial_(initial_ms == 0 ? 1 : initial_ms),
          maximum_(maximum_ms < initial_ ? initial_ : maximum_ms), next_(initial_) {}
    uint32_t next_delay_ms() {
        const uint32_t result = next_;
        next_ = next_ > maximum_ / 2 ? maximum_ : next_ * 2;
        return result;
    }
    void reset() { next_ = initial_; }
private:
    uint32_t initial_;
    uint32_t maximum_;
    uint32_t next_;
};
}  // namespace goldrop
