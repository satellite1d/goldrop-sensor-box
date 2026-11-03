#pragma once
#include "goldrop/managed_config_store.hpp"
#include "goldrop/network_control.hpp"
#include "goldrop/server_control.hpp"
#include "goldrop/device_runtime_control.hpp"
namespace goldrop {
// One owner: telemetry_task. Domain state machine; no ESP-IDF calls.
class ConfigManager final : public ConfigurationControl {
  public:
    ConfigManager(DeviceId id, ManagedConfigStore &store, NetworkControl &wifi, ServerControl &server,
                  DeviceRuntimeControl *runtime = nullptr)
        : id_(id), store_(store), wifi_(wifi), server_(server), runtime_(runtime) {}
    ~ConfigManager() {
        state_.clear();
        legacy_.clear();
        applied_.clear();
    }
    bool begin(const ConfigBundle &legacy, bool legacy_readable);
    bool apply_active(int64_t now);
    void poll(int64_t now, bool transitions_allowed);
    bool accept(const ConfigCommand &, int64_t now);
    bool probe_ready() const;
    bool can_poll(int64_t now) const { return healthy_ && !busy_ && now >= local_hold_until_; }
    bool allows_ota(int64_t now) const {
        return healthy_ && state_.phase != ConfigPhase::Testing && now >= local_hold_until_;
    }
    void set_busy(bool busy) { busy_ = busy; }
    const ManagedConfigState &config_state() const override { return state_; }
    DeviceId configuration_device() const override { return id_; }
    bool configuration_healthy() const override { return healthy_; }
    bool configuration_busy() const override { return busy_; }
    bool local_override(int64_t) override;
    bool local_wifi(const WifiCredentials &, int64_t) override;
    bool local_server(const ServerConfig &, int64_t) override;
    bool local_forget_wifi(int64_t) override;
    bool local_forget_server(int64_t) override;
    bool enroll_management(const ManagementEnrollment &, int64_t) override;
    bool enable_management(bool, int64_t) override;
    bool forget_management(int64_t) override;
    bool recover_configuration(int64_t) override;

  private:
    bool persist(ManagedConfigState &);
    bool apply(const ConfigBundle &, int64_t);
    bool local_commit(ConfigBundle &, int64_t);
    bool rollback(ConfigError, int64_t);
    bool reject(const ConfigCommand &, ConfigError);
    static void clear_candidate(ManagedConfigState &);
    static void record_result(ManagedConfigState &, const ConfigCommand &, ConfigResult, ConfigError);
    DeviceId id_;
    ManagedConfigStore &store_;
    NetworkControl &wifi_;
    ServerControl &server_;
    DeviceRuntimeControl *runtime_{};
    ManagedConfigState state_;
    ConfigBundle legacy_, applied_;
    bool healthy_{}, busy_{}, applied_ready_{}, armed_seen_online_{}, rollback_attempted_{};
    int64_t trial_deadline_{}, local_hold_until_{}, armed_at_{}, lost_at_{-1};
};
} // namespace goldrop
