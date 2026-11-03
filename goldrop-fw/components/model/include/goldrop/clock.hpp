#pragma once
#include <cstdint>

namespace goldrop {
class Clock {
public:
    virtual ~Clock() = default;
    virtual int64_t uptime_ms() const = 0;
    // False means UTC is unknown, never a made-up wall clock time.
    virtual bool utc_ms(int64_t& result) const = 0;
};
}  // namespace goldrop
