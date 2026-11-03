#pragma once
#include "goldrop/wifi_credentials.hpp"
#include "goldrop/wifi_retry_policy.hpp"

namespace goldrop {
struct NetworkStatus {
    WifiState state{WifiState::Unconfigured};
    char ip[16]{"-"};
    int rssi_dbm{};
    bool rssi_valid{};
};
class NetworkControl {
public:
    virtual ~NetworkControl() = default;
    virtual bool configure(const WifiCredentials& config, int64_t now_ms) = 0;
    virtual void disable(int64_t now_ms) = 0;
    virtual NetworkStatus status() const = 0;
};
}  // namespace goldrop
