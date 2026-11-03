#pragma once
#include "goldrop/ota_config.hpp"
#include "goldrop/device_id.hpp"
#include "goldrop/firmware_identity.hpp"
namespace goldrop {
inline constexpr std::size_t kManifestLimit = 8192;
struct Version {
    uint32_t parts[3]{};
    bool parse(std::string_view text);
    int compare(const Version &other) const;
};
struct OtaManifest {
    char version[32]{};
    char hardware[32]{};
    char channel[17]{};
    char url[kOtaUrlCapacity]{};
    std::array<uint8_t, 32> sha256{};
    uint32_t size{};
    bool mandatory{};
    std::array<DeviceId, 32> targets{};
    unsigned target_count{};
    bool targets_present{};
};
bool parse_manifest(std::string_view json, OtaManifest &output);
class ManifestSignatureVerifier {
  public:
    virtual ~ManifestSignatureVerifier() = default;
    virtual bool available() const = 0;
    virtual bool verify(std::string_view payload, const uint8_t *signature, std::size_t size) = 0;
};
bool read_update_manifest(std::string_view json, bool signature_required, ManifestSignatureVerifier &,
                          OtaManifest &, const char *&error);
enum class UpdateDecision {
    Install,
    SameOrOlder,
    WrongHardware,
    WrongChannel,
    WrongDevice,
    TooLarge,
    BadUrl,
    Blocked
};
const char *update_decision_name(UpdateDecision);
UpdateDecision check_update(const OtaManifest &, const OtaConfig &, DeviceId,
                            std::string_view current_version, std::string_view hardware,
                            std::size_t slot_size, const std::array<uint8_t, 32> *blocked);
bool validate_firmware_prefix(const uint8_t *data, std::size_t size, const OtaManifest &manifest);
} // namespace goldrop
