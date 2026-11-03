#pragma once
#include "goldrop/wifi_credentials.hpp"
#include "goldrop/server_config.hpp"
#include "goldrop/ota_config.hpp"
#include "goldrop/device_id.hpp"
#include <array>
#include <limits>
namespace goldrop {
inline constexpr uint64_t kMaxConfigRevision = 0x7fffffffffffffffULL;
inline constexpr int64_t kConfigTrialMs = 120'000;
inline constexpr int64_t kConfigPollMs = 60'000;
inline constexpr int64_t kConfigArmBootGraceMs = 60'000;
inline constexpr int64_t kConfigArmLostMs = 10'000;
enum class ConfigPhase : uint8_t { Idle, Staged, Armed, Testing };
enum class ConfigResult : uint8_t {
    None,
    Staged,
    Armed,
    Testing,
    Committed,
    RolledBack,
    Rejected,
    Cancelled,
    LocalChange
};
enum class ConfigError : uint8_t {
    None,
    BadBase,
    Busy,
    NoCandidate,
    Invalid,
    Timeout,
    Reboot,
    Apply,
    Storage,
    LocalOverride
};
enum class ConfigAction : uint8_t { None, Apply, Stage, Activate, Arm, Cancel, Confirm, Sampling, OtaCheck };
inline const char *config_phase_name(ConfigPhase p) {
    switch (p) {
    case ConfigPhase::Idle:
        return "idle";
    case ConfigPhase::Staged:
        return "staged";
    case ConfigPhase::Armed:
        return "armed";
    case ConfigPhase::Testing:
        return "testing";
    }
    return "unknown";
}
inline const char *config_result_name(ConfigResult p) {
    switch (p) {
    case ConfigResult::None:
        return "none";
    case ConfigResult::Staged:
        return "staged";
    case ConfigResult::Armed:
        return "armed";
    case ConfigResult::Testing:
        return "testing";
    case ConfigResult::Committed:
        return "committed";
    case ConfigResult::RolledBack:
        return "rolled_back";
    case ConfigResult::Rejected:
        return "rejected";
    case ConfigResult::Cancelled:
        return "cancelled";
    case ConfigResult::LocalChange:
        return "local_change";
    }
    return "unknown";
}
inline const char *config_error_name(ConfigError p) {
    switch (p) {
    case ConfigError::None:
        return "none";
    case ConfigError::BadBase:
        return "bad_base";
    case ConfigError::Busy:
        return "busy";
    case ConfigError::NoCandidate:
        return "no_candidate";
    case ConfigError::Invalid:
        return "invalid";
    case ConfigError::Timeout:
        return "timeout";
    case ConfigError::Reboot:
        return "reboot";
    case ConfigError::Apply:
        return "apply";
    case ConfigError::Storage:
        return "storage";
    case ConfigError::LocalOverride:
        return "local_override";
    }
    return "unknown";
}
struct ConfigBundle {
    WifiCredentials wifi;
    ServerConfig server;
    uint8_t present{}; // Wi-Fi=1, telemetry endpoint=2.
    bool valid() const {
        return present <= 3 && (!(present & 1) || wifi.valid()) && (!(present & 2) || server.valid());
    }
    void clear() { clear_secret(this, sizeof(*this)); }
};
struct ManagementEnrollment {
    std::array<char, kOtaUrlCapacity> endpoint{};
    std::array<uint8_t, 32> key{};
    bool enrolled{}, enabled{};
    bool valid() const {
        if (!enrolled)
            return !enabled && endpoint == std::array<char, kOtaUrlCapacity>{} &&
                   key == std::array<uint8_t, 32>{};
        const auto *end = static_cast<const char *>(std::memchr(endpoint.data(), 0, endpoint.size()));
        HttpUrl url;
        return end && key != std::array<uint8_t, 32>{} &&
               url.parse({endpoint.data(), std::size_t(end - endpoint.data())});
    }
    bool set_endpoint(std::string_view text) {
        HttpUrl url;
        if (!url.parse(text))
            return false;
        endpoint.fill(0);
        std::memcpy(endpoint.data(), text.data(), text.size());
        return true;
    }
    void clear() { clear_secret(this, sizeof(*this)); }
};
struct ManagedConfigState {
    uint64_t highwater{}, active_revision{}, pending_revision{}, report_sequence{1}, result_revision{};
    std::array<uint8_t, 16> pending_operation{}, result_operation{};
    ConfigPhase phase{ConfigPhase::Idle};
    ConfigResult result{ConfigResult::None};
    ConfigError error{ConfigError::None};
    ConfigBundle active, candidate;
    ManagementEnrollment management;
    bool valid() const {
        return highwater <= kMaxConfigRevision && active_revision <= highwater &&
               pending_revision <= highwater && result_revision <= highwater && report_sequence &&
               report_sequence <= kMaxConfigRevision && uint8_t(phase) <= uint8_t(ConfigPhase::Testing) &&
               uint8_t(result) <= uint8_t(ConfigResult::LocalChange) &&
               uint8_t(error) <= uint8_t(ConfigError::LocalOverride) && active.valid() && candidate.valid() &&
               management.valid() &&
               (phase == ConfigPhase::Idle
                    ? pending_revision == 0 && pending_operation == std::array<uint8_t, 16>{}
                    : pending_revision > 0 && pending_operation != std::array<uint8_t, 16>{} &&
                          (candidate.present & 1));
    }
    void clear() { clear_secret(this, sizeof(*this)); }
};
struct ConfigCommand {
    ConfigAction action{ConfigAction::None};
    uint64_t revision{}, base_revision{}, target_revision{};
    std::array<uint8_t, 16> operation{};
    ConfigBundle patch;
    uint32_t interval_ms{};
    void clear() { clear_secret(this, sizeof(*this)); }
};
class ConfigurationControl {
  public:
    virtual ~ConfigurationControl() = default;
    virtual const ManagedConfigState &config_state() const = 0;
    virtual DeviceId configuration_device() const = 0;
    virtual bool configuration_healthy() const = 0;
    virtual bool configuration_busy() const = 0;
    virtual bool local_override(int64_t) = 0;
    virtual bool local_wifi(const WifiCredentials &, int64_t) = 0;
    virtual bool local_server(const ServerConfig &, int64_t) = 0;
    virtual bool local_forget_wifi(int64_t) = 0;
    virtual bool local_forget_server(int64_t) = 0;
    virtual bool enroll_management(const ManagementEnrollment &, int64_t) = 0;
    virtual bool enable_management(bool, int64_t) = 0;
    virtual bool forget_management(int64_t) = 0;
    virtual bool recover_configuration(int64_t) = 0;
};
} // namespace goldrop
