#pragma once
#include "goldrop/ota_config.hpp"

namespace goldrop {
enum class OtaState {
    NotConfigured,
    Disabled,
    WaitingNetwork,
    WaitingTime,
    WaitingBoot,
    WaitingStorage,
    WaitingConfiguration,
    Idle,
    Checking,
    Downloading,
    Verifying,
    AwaitingRestart,
    ReadyToReboot,
    Failed
};
inline const char *ota_state_name(OtaState s) {
    switch (s) {
    case OtaState::NotConfigured:
        return "not_configured";
    case OtaState::Disabled:
        return "disabled";
    case OtaState::WaitingNetwork:
        return "waiting_network";
    case OtaState::WaitingTime:
        return "waiting_time";
    case OtaState::WaitingBoot:
        return "waiting_boot";
    case OtaState::WaitingStorage:
        return "waiting_storage";
    case OtaState::WaitingConfiguration:
        return "waiting_configuration";
    case OtaState::Idle:
        return "idle";
    case OtaState::Checking:
        return "checking";
    case OtaState::Downloading:
        return "downloading";
    case OtaState::Verifying:
        return "verifying";
    case OtaState::AwaitingRestart:
        return "awaiting_restart";
    case OtaState::ReadyToReboot:
        return "ready_to_reboot";
    case OtaState::Failed:
        return "failed";
    }
    return "unknown";
}
class OtaService {
  public:
    virtual ~OtaService() = default;
    virtual OtaState state() const = 0;
    // Request only. HTTPS downloads must run outside MQTT callbacks.
    virtual bool request_check() = 0;
    virtual bool configure_ota(const OtaConfig &, int64_t now_ms) = 0;
    virtual void disable_ota() = 0;
    virtual bool clear_block() = 0;
    virtual unsigned progress() const = 0;
    virtual const char *last_error() const = 0;
    virtual bool signed_http_supported() const { return false; }
    virtual const char *public_key_id() const { return "none"; }
};
} // namespace goldrop
