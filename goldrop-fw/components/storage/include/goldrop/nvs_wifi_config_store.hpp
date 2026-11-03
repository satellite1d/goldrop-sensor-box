#pragma once
#include "goldrop/wifi_config_store.hpp"

namespace goldrop {
class NvsWifiConfigStore final : public WifiConfigStore {
public:
    ConfigLoadResult load_wifi(WifiCredentials& result) override;
    bool save_wifi(const WifiCredentials& config) override;
    bool forget_wifi() override;
};
}  // namespace goldrop
