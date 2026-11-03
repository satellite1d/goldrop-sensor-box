#pragma once
#include "goldrop/clock.hpp"
#include <atomic>
namespace goldrop {
class SystemClock final : public Clock {
  public:
    ~SystemClock() override;
    int64_t uptime_ms() const override;
    bool utc_ms(int64_t &) const override;
    void poll(bool wifi_ready);
    bool synchronized() const;

  private:
    static void sync_event(void *, const char *, int32_t, void *);
    std::atomic<bool> synchronized_{false};
    void *handler_{};
    bool initialized_{};
    bool wifi_was_ready_{};
    bool restart_pending_{true};
    int64_t next_attempt_{};
};
} // namespace goldrop
