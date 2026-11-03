#pragma once
#include "goldrop/network_control.hpp"
#include <memory>

namespace goldrop {
// Called only by telemetry_task. ESP-IDF callbacks enqueue short events.
class WifiManager final : public NetworkControl {
public:
    WifiManager();
    ~WifiManager() override;
    bool configure(const WifiCredentials& config, int64_t now_ms) override;
    void disable(int64_t now_ms) override;
    NetworkStatus status() const override;
    void poll(int64_t now_ms);
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
}  // namespace goldrop
