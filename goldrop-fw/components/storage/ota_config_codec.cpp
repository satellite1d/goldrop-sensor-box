#include "goldrop/ota_config_store.hpp"
#include "goldrop/binary_codec.hpp"
namespace goldrop {
bool encode_ota_config(const OtaConfig &c, OtaConfigBlob &b) {
    b.fill(0);
    if (!c.valid())
        return false;
    std::memcpy(b.data(), "GOTA", 4);
    b[4] = 2;
    b[5] = c.enabled ? 1 : 0;
    binary::put32(b.data() + 8, c.interval_seconds);
    std::memcpy(b.data() + 12, c.manifest_url.data(), c.manifest_url.size());
    std::memcpy(b.data() + 396, c.channel.data(), c.channel.size());
    return true;
}
bool decode_ota_config(const uint8_t *b, std::size_t n, OtaConfig &c) {
    c = {};
    if (!b || n != OtaConfigBlob{}.size() || std::memcmp(b, "GOTA", 4) || (b[4] != 1 && b[4] != 2) ||
        b[5] > 1 || b[6] || b[7] || b[413] || b[414] || b[415])
        return false;
    OtaConfig candidate;
    candidate.enabled = b[5];
    candidate.interval_seconds = binary::get32(b + 8);
    std::memcpy(candidate.manifest_url.data(), b + 12, candidate.manifest_url.size());
    std::memcpy(candidate.channel.data(), b + 396, candidate.channel.size());
    HttpsUrl legacy_url;
    if (!candidate.valid() || (b[4] == 1 && !legacy_url.parse(candidate.manifest_url.data())))
        return false;
    c = candidate;
    return true;
}
} // namespace goldrop
