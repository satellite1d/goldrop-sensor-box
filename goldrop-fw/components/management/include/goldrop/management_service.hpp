#pragma once
#include "goldrop/config_manager.hpp"
#include "goldrop/management_protocol.hpp"
#include <atomic>
namespace goldrop {
// Lifetime is the application lifetime. Only telemetry_task calls poll().
// At most one bounded HTTP worker exists, mutually exclusive with OTA.
class ManagementService {
  public:
    ManagementService(DeviceId id, ConfigManager &config) : id_(id), config_(config) {}
    void poll(int64_t now, bool network, bool time, bool allowed);
    bool active() const { return active_; }

  private:
    static void task_entry(void *);
    void exchange();
    DeviceId id_;
    ConfigManager &config_;
    ManagedConfigState snapshot_;
    ManagementChallenge challenge_{};
    ConfigCommand reply_;
    std::atomic<bool> done_{false};
    bool active_{}, success_{}, probe_{};
    uint64_t observed_report_{};
    uint64_t exchange_sequence_{};
    bool observed_probe_{};
    int64_t next_{};
    unsigned failures_{};
};
} // namespace goldrop
