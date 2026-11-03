#include "goldrop/wifi_config_codec.hpp"

namespace goldrop {
bool encode_wifi_config(const WifiCredentials& config, WifiConfigBlob& output) {
    output.fill(0);
    if (!config.valid()) return false;
    const auto ssid_size = std::strlen(config.ssid.data());
    const auto password_size = std::strlen(config.password.data());
    output[0] = 'G'; output[1] = 'W'; output[2] = 'F'; output[3] = 'I';
    output[4] = 1;
    output[5] = static_cast<uint8_t>(ssid_size);
    output[6] = static_cast<uint8_t>(password_size);
    std::memcpy(output.data() + 8, config.ssid.data(), ssid_size);
    std::memcpy(output.data() + 40, config.password.data(), password_size);
    return true;
}

bool decode_wifi_config(const uint8_t* bytes, std::size_t size, WifiCredentials& output) {
    output.clear();
    if (!bytes || size != WifiConfigBlob{}.size() || bytes[0] != 'G' || bytes[1] != 'W' ||
        bytes[2] != 'F' || bytes[3] != 'I' || bytes[4] != 1 || bytes[7] != 0) return false;
    WifiCredentials decoded;
    const bool valid = decoded.set_ssid(reinterpret_cast<const char*>(bytes + 8), bytes[5]) &&
                       decoded.set_password(reinterpret_cast<const char*>(bytes + 40), bytes[6]);
    if (valid) output = decoded;
    decoded.clear();
    return valid;
}
}  // namespace goldrop
