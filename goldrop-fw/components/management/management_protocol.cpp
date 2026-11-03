#include "goldrop/management_protocol.hpp"
#include "goldrop/binary_codec.hpp"
#include "goldrop/firmware_identity.hpp"
namespace goldrop {
void encode_management_request(const ManagedConfigState &s, const ManagementChallenge &challenge, bool wifi,
                               bool probe, uint64_t exchange_sequence, ManagementRequest &out) {
    out.fill(0);
    std::memcpy(out.data(), "GQ01", 4);
    out[4] = uint8_t(s.phase);
    out[5] = uint8_t(s.result);
    out[6] = uint8_t(s.error);
    out[7] = (wifi ? 1 : 0) | (probe ? 2 : 0);
    binary::put64(out.data() + 8, s.report_sequence);
    binary::put64(out.data() + 16, s.highwater);
    binary::put64(out.data() + 24, s.active_revision);
    binary::put64(out.data() + 32, s.pending_revision);
    binary::put64(out.data() + 40, s.result_revision);
    std::memcpy(out.data() + 48, s.result_operation.data(), 16);
    std::memcpy(out.data() + 64, s.pending_operation.data(), 16);
    std::memcpy(out.data() + 80, challenge.data(), 16);
    std::memcpy(out.data() + 96, kFirmwareVersion, std::strlen(kFirmwareVersion));
    binary::put64(out.data() + 128, exchange_sequence);
}
bool decode_management_response(const ManagementResponse &in, const ManagementChallenge &challenge,
                                ConfigCommand &out) {
    out.clear();
    if (std::memcmp(in.data(), "GP01", 4) || std::memcmp(in.data() + 48, challenge.data(), 16) || in[4] > 8 ||
        in[5] > 3 || in[6] || in[7])
        return false;
    for (std::size_t i = 421; i < in.size(); ++i)
        if (in[i])
            return false;
    ConfigCommand c;
    c.action = ConfigAction(in[4]);
    c.interval_ms = binary::get32(in.data() + 417);
    c.patch.present = in[5];
    c.revision = binary::get64(in.data() + 8);
    c.base_revision = binary::get64(in.data() + 16);
    c.target_revision = binary::get64(in.data() + 24);
    std::memcpy(c.operation.data(), in.data() + 32, 16);
    const auto read_string = [&](std::size_t offset, std::size_t size, const auto &set) {
        const auto *p = reinterpret_cast<const char *>(in.data() + offset);
        const auto *end = static_cast<const char *>(std::memchr(p, 0, size));
        if (!end)
            return false;
        for (std::size_t i = std::size_t(end - p); i < size; ++i)
            if (p[i])
                return false;
        return set(p, std::size_t(end - p));
    };
    bool ok = c.revision <= kMaxConfigRevision && c.base_revision <= kMaxConfigRevision &&
              c.target_revision <= kMaxConfigRevision;
    if (c.patch.present & 1) {
        ok = ok &&
             read_string(64, 33, [&](const char *p, std::size_t n) { return c.patch.wifi.set_ssid(p, n); }) &&
             read_string(97, 64,
                         [&](const char *p, std::size_t n) { return c.patch.wifi.set_password(p, n); });
    } else {
        for (std::size_t i = 64; i < 161; ++i)
            ok = ok && !in[i];
    }
    if (c.patch.present & 2) {
        ok = ok && read_string(161, 254, [&](const char *p, std::size_t n) {
                 return c.patch.server.set_address(p, n);
             });
        c.patch.server.port = uint16_t(in[415]) | (uint16_t(in[416]) << 8);
        ok = ok && c.patch.server.valid();
    } else {
        for (std::size_t i = 161; i < 417; ++i)
            ok = ok && !in[i];
    }
    ok = ok && (c.action == ConfigAction::Sampling ? c.interval_ms >= settings::kMinimumSamplingIntervalMs &&
                                                         c.interval_ms <= settings::kMaximumSamplingIntervalMs
                                                   : c.interval_ms == 0);
    if (c.action == ConfigAction::None)
        ok = ok && !c.revision && !c.base_revision && !c.target_revision && !c.patch.present &&
             c.operation == ManagementChallenge{};
    else
        ok = ok && c.revision && c.operation != ManagementChallenge{};
    const bool patch = c.action == ConfigAction::Apply || c.action == ConfigAction::Stage;
    const bool standalone = c.action == ConfigAction::Sampling || c.action == ConfigAction::OtaCheck;
    if (standalone)
        ok = ok && !c.patch.present && !c.target_revision;
    else if (c.action != ConfigAction::None)
        ok = ok &&
             (patch ? c.patch.present > 0 && !c.target_revision : !c.patch.present && c.target_revision > 0);
    if (ok)
        out = c;
    c.clear();
    return ok;
}
} // namespace goldrop
