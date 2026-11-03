#include "goldrop/managed_config_store.hpp"
#include "goldrop/wifi_config_codec.hpp"
#include "goldrop/server_config_store.hpp"
#include "goldrop/binary_codec.hpp"
namespace goldrop {
namespace {
bool encode_bundle(const ConfigBundle &c, uint8_t *out) {
    WifiConfigBlob wifi{};
    ServerConfigBlob server{};
    const bool ok = (!(c.present & 1) || encode_wifi_config(c.wifi, wifi)) &&
                    (!(c.present & 2) || encode_server_config(c.server, server));
    if (ok) {
        std::memcpy(out, wifi.data(), 104);
        std::memcpy(out + 104, server.data(), 264);
    }
    clear_secret(wifi.data(), wifi.size());
    clear_secret(server.data(), server.size());
    return ok;
}
bool decode_bundle(const uint8_t *b, uint8_t mask, ConfigBundle &c) {
    c.present = mask;
    if (mask > 3)
        return false;
    if (mask & 1) {
        if (!decode_wifi_config(b, 104, c.wifi))
            return false;
    } else
        for (unsigned i = 0; i < 104; ++i)
            if (b[i])
                return false;
    if (mask & 2) {
        if (!decode_server_config(b + 104, 264, c.server))
            return false;
    } else
        for (unsigned i = 104; i < 368; ++i)
            if (b[i])
                return false;
    return true;
}
} // namespace
bool encode_managed_config(const ManagedConfigState &s, ManagedConfigBlob &b) {
    b.fill(0);
    if (!s.valid())
        return false;
    std::memcpy(b.data(), "GCF1", 4);
    b[4] = 1;
    binary::put64(b.data() + 8, s.highwater);
    binary::put64(b.data() + 16, s.active_revision);
    binary::put64(b.data() + 24, s.pending_revision);
    binary::put64(b.data() + 32, s.report_sequence);
    binary::put64(b.data() + 40, s.result_revision);
    std::memcpy(b.data() + 48, s.pending_operation.data(), 16);
    std::memcpy(b.data() + 64, s.result_operation.data(), 16);
    b[80] = uint8_t(s.phase);
    b[81] = uint8_t(s.result);
    b[82] = uint8_t(s.error);
    b[83] = s.active.present;
    b[84] = s.candidate.present;
    if (!encode_bundle(s.active, b.data() + 96) || !encode_bundle(s.candidate, b.data() + 464))
        return false;
    std::memcpy(b.data() + 832, s.management.endpoint.data(), 384);
    std::memcpy(b.data() + 1216, s.management.key.data(), 32);
    b[1248] = (s.management.enrolled ? 1 : 0) | (s.management.enabled ? 2 : 0);
    binary::put32(b.data() + 1276, binary::crc32(b.data(), 1276));
    return true;
}
bool decode_managed_config(const uint8_t *b, std::size_t size, ManagedConfigState &s) {
    s.clear();
    if (!b || size != 1280 || std::memcmp(b, "GCF1", 4) || b[4] != 1 || b[5] || b[6] || b[7] ||
        binary::get32(b + 1276) != binary::crc32(b, 1276))
        return false;
    for (unsigned i = 85; i < 96; ++i)
        if (b[i])
            return false;
    for (unsigned i = 1249; i < 1276; ++i)
        if (b[i])
            return false;
    if (b[1248] > 3 || b[1248] == 2)
        return false;
    s.highwater = binary::get64(b + 8);
    s.active_revision = binary::get64(b + 16);
    s.pending_revision = binary::get64(b + 24);
    s.report_sequence = binary::get64(b + 32);
    s.result_revision = binary::get64(b + 40);
    std::memcpy(s.pending_operation.data(), b + 48, 16);
    std::memcpy(s.result_operation.data(), b + 64, 16);
    s.phase = ConfigPhase(b[80]);
    s.result = ConfigResult(b[81]);
    s.error = ConfigError(b[82]);
    if (!decode_bundle(b + 96, b[83], s.active) || !decode_bundle(b + 464, b[84], s.candidate)) {
        s.clear();
        return false;
    }
    std::memcpy(s.management.endpoint.data(), b + 832, 384);
    std::memcpy(s.management.key.data(), b + 1216, 32);
    s.management.enrolled = b[1248] & 1;
    s.management.enabled = b[1248] & 2;
    if (!s.valid()) {
        s.clear();
        return false;
    }
    return true;
}
} // namespace goldrop
