#include "goldrop/config_manager.hpp"
namespace goldrop {
void ConfigManager::clear_candidate(ManagedConfigState &s) {
    s.candidate.clear();
    s.pending_revision = 0;
    s.pending_operation.fill(0);
    s.phase = ConfigPhase::Idle;
}
void ConfigManager::record_result(ManagedConfigState &s, const ConfigCommand &c, ConfigResult result,
                                  ConfigError error) {
    s.result_revision = c.revision;
    s.result_operation = c.operation;
    s.result = result;
    s.error = error;
}
bool ConfigManager::persist(ManagedConfigState &next) {
    if (next.report_sequence >= kMaxConfigRevision) {
        next.clear();
        healthy_ = false;
        return false;
    }
    ++next.report_sequence;
    const bool ok = next.valid() && store_.save(next);
    if (ok) {
        state_.clear();
        state_ = next;
        healthy_ = true;
    } else
        healthy_ = false;
    next.clear();
    return ok;
}
bool ConfigManager::begin(const ConfigBundle &legacy, bool legacy_readable) {
    legacy_ = legacy;
    const auto result = store_.load(state_);
    if (result == ConfigLoadResult::Failed) {
        state_ = ManagedConfigState{};
        state_.active = legacy;
        return false;
    }
    if (result == ConfigLoadResult::Defaults) {
        state_ = ManagedConfigState{};
        state_.active = legacy;
        if (!legacy_readable || !legacy.valid())
            return false;
    }
    auto next = state_;
    if (next.phase == ConfigPhase::Testing) {
        next.result = ConfigResult::RolledBack;
        next.error = ConfigError::Reboot;
        next.result_revision = next.pending_revision;
        next.result_operation = next.pending_operation;
        clear_candidate(next);
    }
    return persist(next); // A durable report sequence orders status across boots.
}
bool ConfigManager::apply(const ConfigBundle &bundle, int64_t now) {
    const bool wifi_changed = !applied_ready_ || ((bundle.present ^ applied_.present) & 1) ||
                              bundle.wifi.ssid != applied_.wifi.ssid ||
                              bundle.wifi.password != applied_.wifi.password;
    const bool server_changed =
        !applied_ready_ || wifi_changed || ((bundle.present ^ applied_.present) & 2) ||
        bundle.server.host != applied_.server.host || bundle.server.port != applied_.server.port;
    if (server_changed) {
        if (bundle.present & 2) {
            if (!server_.configure_server(bundle.server, now)) {
                applied_ready_ = false;
                return false;
            }
        } else
            server_.disable_server();
    }
    if (wifi_changed) {
        if (bundle.present & 1) {
            if (!wifi_.configure(bundle.wifi, now)) {
                applied_ready_ = false;
                return false;
            }
        } else
            wifi_.disable(now);
    }
    applied_.clear();
    applied_ = bundle;
    applied_ready_ = true;
    return true;
}
bool ConfigManager::apply_active(int64_t now) { return apply(state_.active, now); }
bool ConfigManager::probe_ready() const {
    return wifi_.status().state == WifiState::Online &&
           (!(state_.candidate.present & 2) || server_.backend_ready());
}
bool ConfigManager::rollback(ConfigError reason, int64_t now) {
    rollback_attempted_ = true;
    auto next = state_;
    next.result = ConfigResult::RolledBack;
    next.error = reason;
    next.result_revision = next.pending_revision;
    next.result_operation = next.pending_operation;
    clear_candidate(next);
    const bool stored = persist(next);
    const bool applied = apply_active(now); // Restore reachability even if a subsequent NVS write failed.
    return stored && applied;
}
bool ConfigManager::reject(const ConfigCommand &c, ConfigError error) {
    auto next = state_;
    next.highwater = c.revision;
    record_result(next, c, ConfigResult::Rejected, error);
    return persist(next);
}
bool ConfigManager::accept(const ConfigCommand &c, int64_t now) {
    if (!can_poll(now))
        return false;
    if (c.action == ConfigAction::Confirm) {
        if (state_.phase != ConfigPhase::Testing || now >= trial_deadline_ || !probe_ready() ||
            c.revision != state_.pending_revision || c.base_revision != state_.active_revision ||
            c.target_revision != state_.pending_revision || c.operation != state_.pending_operation)
            return false;
        auto next = state_;
        next.active = next.candidate;
        next.active_revision = next.pending_revision;
        next.result = ConfigResult::Committed;
        next.error = ConfigError::None;
        next.result_revision = next.pending_revision;
        next.result_operation = next.pending_operation;
        clear_candidate(next);
        return persist(next);
    }
    if (c.action == ConfigAction::None || !c.revision || c.revision <= state_.highwater ||
        c.revision > kMaxConfigRevision || c.operation == std::array<uint8_t, 16>{})
        return false;
    if (c.base_revision != state_.active_revision)
        return reject(c, ConfigError::BadBase);
    if (c.action == ConfigAction::Sampling || c.action == ConfigAction::OtaCheck) {
        if (state_.phase != ConfigPhase::Idle)
            return reject(c, ConfigError::Busy);
        if (!runtime_ || c.patch.present || c.target_revision ||
            (c.action == ConfigAction::Sampling ? c.interval_ms < settings::kMinimumSamplingIntervalMs ||
                                                      c.interval_ms > settings::kMaximumSamplingIntervalMs
                                                : c.interval_ms != 0))
            return reject(c, ConfigError::Invalid);
        const bool applied = c.action == ConfigAction::Sampling
                                 ? runtime_->set_sampling_interval(c.interval_ms)
                                 : runtime_->request_firmware_check();
        auto next = state_;
        next.highwater = c.revision;
        record_result(next, c, applied ? ConfigResult::Committed : ConfigResult::Rejected,
                      applied ? ConfigError::None : ConfigError::Apply);
        return persist(next);
    }
    if (c.action == ConfigAction::Apply || c.action == ConfigAction::Stage) {
        if (state_.phase != ConfigPhase::Idle)
            return reject(c, ConfigError::Busy);
        if (!c.patch.present || !c.patch.valid())
            return reject(c, ConfigError::Invalid);
        auto next = state_;
        next.candidate = next.active;
        if (c.patch.present & 1) {
            next.candidate.wifi = c.patch.wifi;
            next.candidate.present |= 1;
        }
        if (c.patch.present & 2) {
            next.candidate.server = c.patch.server;
            next.candidate.present |= 2;
        }
        if (!(next.candidate.present & 1)) {
            next.clear();
            return reject(c, ConfigError::Invalid);
        }
        next.highwater = c.revision;
        next.pending_revision = c.revision;
        next.pending_operation = c.operation;
        next.phase = c.action == ConfigAction::Apply ? ConfigPhase::Testing : ConfigPhase::Staged;
        record_result(next, c, c.action == ConfigAction::Apply ? ConfigResult::Testing : ConfigResult::Staged,
                      ConfigError::None);
        if (!persist(next))
            return false;
        if (c.action == ConfigAction::Apply) {
            rollback_attempted_ = false;
            trial_deadline_ = now + kConfigTrialMs;
            if (!apply(state_.candidate, now))
                return rollback(ConfigError::Apply, now);
        }
        return true;
    }
    if (c.action != ConfigAction::Activate && c.action != ConfigAction::Arm &&
        c.action != ConfigAction::Cancel)
        return reject(c, ConfigError::Invalid);
    if (state_.phase == ConfigPhase::Idle || c.target_revision != state_.pending_revision)
        return reject(c, ConfigError::NoCandidate);
    if (state_.phase == ConfigPhase::Testing && c.action != ConfigAction::Cancel)
        return reject(c, ConfigError::Busy);
    const bool was_testing = state_.phase == ConfigPhase::Testing;
    auto next = state_;
    next.highwater = c.revision;
    if (c.action == ConfigAction::Cancel) {
        clear_candidate(next);
        record_result(next, c, ConfigResult::Cancelled, ConfigError::None);
    } else {
        next.pending_revision = c.revision;
        next.pending_operation = c.operation;
        next.phase = c.action == ConfigAction::Arm ? ConfigPhase::Armed : ConfigPhase::Testing;
        record_result(next, c, c.action == ConfigAction::Arm ? ConfigResult::Armed : ConfigResult::Testing,
                      ConfigError::None);
    }
    if (!persist(next))
        return false;
    if (c.action == ConfigAction::Cancel)
        return !was_testing || apply_active(now);
    if (c.action == ConfigAction::Arm) {
        armed_at_ = now;
        armed_seen_online_ = wifi_.status().state == WifiState::Online;
        lost_at_ = -1;
        return true;
    }
    trial_deadline_ = now + kConfigTrialMs;
    rollback_attempted_ = false;
    return apply(state_.candidate, now) || rollback(ConfigError::Apply, now);
}
void ConfigManager::poll(int64_t now, bool allowed) {
    if (state_.phase == ConfigPhase::Testing) {
        if (now >= trial_deadline_ && !rollback_attempted_)
            rollback(ConfigError::Timeout, now);
        return;
    }
    if (!healthy_)
        return;
    if (!allowed || busy_ || now < local_hold_until_ || state_.phase != ConfigPhase::Armed)
        return;
    if (wifi_.status().state == WifiState::Online) {
        armed_seen_online_ = true;
        lost_at_ = -1;
        return;
    }
    if (lost_at_ < 0)
        lost_at_ = now;
    if ((armed_seen_online_ && now - lost_at_ < kConfigArmLostMs) ||
        (!armed_seen_online_ && now - armed_at_ < kConfigArmBootGraceMs))
        return;
    auto next = state_;
    next.phase = ConfigPhase::Testing;
    next.result = ConfigResult::Testing;
    next.error = ConfigError::None;
    if (!persist(next))
        return;
    trial_deadline_ = now + kConfigTrialMs;
    rollback_attempted_ = false;
    if (!apply(state_.candidate, now))
        rollback(ConfigError::Apply, now);
}
bool ConfigManager::local_override(int64_t now) {
    if (busy_)
        return false;
    local_hold_until_ = now + 30'000;
    if (state_.phase == ConfigPhase::Idle)
        return true;
    return healthy_ && rollback(ConfigError::LocalOverride, now);
}
bool ConfigManager::local_commit(ConfigBundle &bundle, int64_t now) {
    if (busy_ || !healthy_ || !bundle.valid() || state_.highwater >= kMaxConfigRevision) {
        bundle.clear();
        return false;
    }
    auto next = state_;
    next.active = bundle;
    bundle.clear();
    next.active_revision = ++next.highwater;
    clear_candidate(next);
    next.result = ConfigResult::LocalChange;
    next.error = ConfigError::None;
    next.result_revision = next.highwater;
    next.result_operation.fill(0);
    if (!persist(next))
        return false;
    local_hold_until_ = now + 30'000;
    return apply_active(now);
}
bool ConfigManager::local_wifi(const WifiCredentials &wifi, int64_t now) {
    auto bundle = state_.active;
    bundle.wifi = wifi;
    bundle.present |= 1;
    return local_commit(bundle, now);
}
bool ConfigManager::local_server(const ServerConfig &server, int64_t now) {
    auto bundle = state_.active;
    bundle.server = server;
    bundle.present |= 2;
    return local_commit(bundle, now);
}
bool ConfigManager::local_forget_wifi(int64_t now) {
    auto bundle = state_.active;
    bundle.wifi.clear();
    bundle.present &= ~1;
    return local_commit(bundle, now);
}
bool ConfigManager::local_forget_server(int64_t now) {
    auto bundle = state_.active;
    bundle.server = {};
    bundle.present &= ~2;
    return local_commit(bundle, now);
}
bool ConfigManager::enroll_management(const ManagementEnrollment &enrollment, int64_t now) {
    if (!healthy_ || busy_ || !enrollment.enrolled || !enrollment.valid() || !local_override(now))
        return false;
    auto next = state_;
    next.management.clear();
    next.management = enrollment;
    return persist(next);
}
bool ConfigManager::enable_management(bool enabled, int64_t now) {
    if (!healthy_ || busy_ || !state_.management.enrolled || !local_override(now))
        return false;
    auto next = state_;
    next.management.enabled = enabled;
    return persist(next);
}
bool ConfigManager::forget_management(int64_t now) {
    if (!healthy_ || busy_ || !local_override(now))
        return false;
    auto next = state_;
    next.management.clear();
    return persist(next);
}
bool ConfigManager::recover_configuration(int64_t now) {
    if (busy_)
        return false;
    auto next = ManagedConfigState{}; // Explicit USB recovery removes the old key as well as its counters.
    next.active = legacy_.valid() ? legacy_ : ConfigBundle{};
    if (!persist(next))
        return false;
    local_hold_until_ = now + 30'000;
    return apply_active(now);
}
} // namespace goldrop
