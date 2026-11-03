#pragma once

#include "goldrop/wifi_credentials.hpp"

namespace goldrop {
// Explicit v1 layout, independent of struct padding. NVS supplies integrity checking.
// Header (magic/version/lengths/reserved) + SSID (32 bytes) + password (64 bytes).
using WifiConfigBlob = std::array<uint8_t, 104>;
bool encode_wifi_config(const WifiCredentials& config, WifiConfigBlob& output);
bool decode_wifi_config(const uint8_t* bytes, std::size_t size, WifiCredentials& output);
}  // namespace goldrop
