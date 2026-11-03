#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace goldrop {
inline constexpr std::size_t kUtcTimestampCapacity = 25; // YYYY-MM-DDTHH:MM:SS.mmmZ + NUL

// Formats the recorded instant, independent of local timezone and current clock.
inline bool format_utc_timestamp(int64_t timestamp_ms, char *output, std::size_t capacity) {
    if (!output || !capacity)
        return false;
    output[0] = '\0';
    // Four-digit years and nonnegative Unix time; bound conversion before time_t cast.
    if (capacity < kUtcTimestampCapacity || timestamp_ms < 0 || timestamp_ms > 253402300799999LL)
        return false;
    const int64_t seconds = timestamp_ms / 1000;
    const auto epoch = static_cast<time_t>(seconds);
    if (static_cast<int64_t>(epoch) != seconds)
        return false;
    std::tm utc{};
    if (!gmtime_r(&epoch, &utc))
        return false;
    char date[20];
    if (std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &utc) != 19)
        return false;
    const int length =
        std::snprintf(output, capacity, "%s.%03uZ", date, static_cast<unsigned>(timestamp_ms % 1000));
    if (length != static_cast<int>(kUtcTimestampCapacity - 1)) {
        output[0] = '\0';
        return false;
    }
    return true;
}
} // namespace goldrop
