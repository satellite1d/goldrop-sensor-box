#pragma once
#include <array>
#include <cstdint>

namespace goldrop {
// Wi-Fi station MAC, captured at boot. No dynamic allocation or dependency on Wi-Fi state.
struct DeviceId {
    std::array<uint8_t, 6> mac{};
    bool valid() const {
        uint8_t combined = 0;
        for (const auto byte : mac) combined |= byte;
        return combined != 0 && (mac[0] & 1) == 0;
    }
    bool format(char (&output)[18]) const {
        output[0] = 0;
        if (!valid()) return false;
        constexpr char hex[] = "0123456789abcdef";
        for (unsigned i = 0; i < mac.size(); ++i) {
            output[3 * i] = hex[mac[i] >> 4];
            output[3 * i + 1] = hex[mac[i] & 15];
            output[3 * i + 2] = i == mac.size() - 1 ? '\0' : ':';
        }
        return true;
    }
    bool operator==(const DeviceId& other) const { return mac == other.mac; }
};
}  // namespace goldrop
