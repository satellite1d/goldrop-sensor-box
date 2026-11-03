#pragma once

#include "goldrop/config_store.hpp"
#include "goldrop/wifi_credentials.hpp"

namespace goldrop {
class WifiConfigStore {
public:
    virtual ~WifiConfigStore() = default;
    virtual ConfigLoadResult load_wifi(WifiCredentials& result) = 0;
    virtual bool save_wifi(const WifiCredentials& config) = 0;
    virtual bool forget_wifi() = 0;
};
}  // namespace goldrop
