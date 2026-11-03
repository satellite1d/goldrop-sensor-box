#include "goldrop/server_config_store.hpp"
#include <cstdio>

namespace goldrop {
bool encode_server_config(const ServerConfig& config, ServerConfigBlob& blob) {
    blob.fill(0);
    if (!config.valid()) return false;
    blob[0] = 'G'; blob[1] = 'T'; blob[2] = 'C'; blob[3] = 'P';
    blob[4] = 2;
    const auto size = std::strlen(config.host.data());
    blob[5] = static_cast<uint8_t>(size);
    blob[6] = static_cast<uint8_t>(config.port >> 8);
    blob[7] = static_cast<uint8_t>(config.port);
    std::memcpy(blob.data() + 8, config.host.data(), size);
    return true;
}
bool decode_server_config(const uint8_t* bytes, std::size_t size, ServerConfig& config) {
    config = {};
    if (!bytes || size < 8 || bytes[0] != 'G' || bytes[1] != 'T' || bytes[2] != 'C' || bytes[3] != 'P') return false;
    ServerConfig candidate;
    candidate.port = static_cast<uint16_t>((bytes[6] << 8) | bytes[7]);
    if (size == LegacyServerConfigBlob{}.size() && bytes[4] == 1 && bytes[5] == 0) {
        char address[16];
        const int count = std::snprintf(address, sizeof(address), "%u.%u.%u.%u", bytes[8], bytes[9], bytes[10], bytes[11]);
        if (count <= 0 || count >= static_cast<int>(sizeof(address)) ||
            !candidate.set_address(address, static_cast<std::size_t>(count))) return false;
    } else if (size == ServerConfigBlob{}.size() && bytes[4] == 2) {
        if (!candidate.set_address(reinterpret_cast<const char*>(bytes + 8), bytes[5])) return false;
        for (std::size_t i = 8 + bytes[5]; i < size; ++i) if (bytes[i] != 0) return false;
    } else return false;
    if (!candidate.valid()) return false;
    config = candidate;
    return true;
}
}  // namespace goldrop
