#pragma once
#include "goldrop/ota_service.hpp"
#include "goldrop/ota_config_store.hpp"
#include "goldrop/ota_manifest.hpp"
#include "goldrop/backoff.hpp"
#include "goldrop/manifest_signature.hpp"
#include <atomic>
namespace goldrop {
// Runtime owns this service until reboot. Only the owner task configures/polls it.
class OtaManager final : public OtaService {
  public:
    explicit OtaManager(DeviceId id) : device_(id) {}
    ~OtaManager() override;
    bool begin();
    void poll(int64_t now, bool network, bool time, bool boot_ready, bool storage_ready,
              bool configuration_ready = true);
    bool busy() const {
        return active_.load(std::memory_order_acquire) || state() == OtaState::AwaitingRestart ||
               state() == OtaState::ReadyToReboot;
    }
    OtaState state() const override { return state_.load(std::memory_order_acquire); }
    unsigned progress() const override { return progress_.load(std::memory_order_relaxed); }
    const char *last_error() const override { return error_.load(std::memory_order_acquire); }
    bool signed_http_supported() const override { return signature_.available(); }
    const char *public_key_id() const override { return signature_.key_id(); }
    bool request_check() override;
    bool configure_ota(const OtaConfig &, int64_t now_ms) override;
    void disable_ota() override;
    bool clear_block() override;
    void allow_restart() { restart_allowed_.store(true, std::memory_order_release); }
    void cancel() { cancel_.store(true, std::memory_order_release); }

  private:
    static void task_entry(void *);
    void run();
    bool healthy_request() const;
    void fail(const char *);
    DeviceId device_;
    OtaConfig config_, work_config_;
    OtaAttempt attempt_, work_attempt_;
    OtaAttemptStore attempts_;
    MbedtlsManifestVerifier signature_;
    std::atomic<OtaState> state_{OtaState::NotConfigured};
    std::atomic<unsigned> progress_{0};
    std::atomic<const char *> error_{"none"};
    std::atomic<bool> active_{false}, cancel_{false}, restart_allowed_{false}, network_{false},
        storage_{false};
    std::atomic<unsigned> completions_{0};
    std::atomic<bool> success_{false};
    unsigned observed_completion_{};
    bool initialized_{}, configured_{}, requested_{}, attempt_reconciled_{};
    int64_t next_check_{};
    int64_t next_reconcile_{};
    Backoff backoff_{300000, 3600000};
};
} // namespace goldrop
