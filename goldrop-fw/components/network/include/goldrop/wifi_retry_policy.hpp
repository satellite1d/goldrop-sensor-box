#pragma once
#include "goldrop/backoff.hpp"
#include <cstdint>

namespace goldrop {
enum class WifiState { Unconfigured, Starting, Connecting, WaitingIp, Online, Backoff, Stopping };
inline const char* wifi_state_name(WifiState state) {
    switch (state) {
        case WifiState::Unconfigured: return "unconfigured";
        case WifiState::Starting: return "starting";
        case WifiState::Connecting: return "connecting";
        case WifiState::WaitingIp: return "waiting_ip";
        case WifiState::Online: return "online";
        case WifiState::Backoff: return "backoff";
        case WifiState::Stopping: return "stopping";
    }
    return "unknown";
}
enum class WifiAction { None, Retry, Disconnect, Stop };
class WifiRetryPolicy {
public:
    void reset() { state_ = WifiState::Unconfigured; backoff_.reset(); }
    void stopping(int64_t now) { state_ = WifiState::Stopping; deadline_ = now + 5'000; }
    void starting(int64_t now) { state_ = WifiState::Starting; deadline_ = now + 5'000; }
    void started(int64_t now) {
        if (state_ == WifiState::Starting) { state_ = WifiState::Backoff; deadline_ = now; }
    }
    void associated(int64_t now) {
        if (state_ == WifiState::Connecting) { state_ = WifiState::WaitingIp; deadline_ = now + 15'000; }
    }
    bool got_ip(int64_t now) {
        if (state_ != WifiState::WaitingIp && state_ != WifiState::Online) return false;
        if (state_ != WifiState::Online) online_since_ = now;
        state_ = WifiState::Online;
        return true;
    }
    void failed(int64_t now, uint32_t jitter = 0) {
        if (state_ == WifiState::Unconfigured || state_ == WifiState::Stopping ||
            state_ == WifiState::Backoff) return;
        if (state_ == WifiState::Online && now - online_since_ >= 30'000) backoff_.reset();
        schedule(now, jitter);
    }
    WifiAction poll(int64_t now, uint32_t jitter = 0) {
        if (state_ == WifiState::Online && now - online_since_ >= 30'000) backoff_.reset();
        if (state_ == WifiState::Unconfigured || state_ == WifiState::Online || now < deadline_) {
            return WifiAction::None;
        }
        if (state_ == WifiState::Backoff) {
            state_ = WifiState::Connecting;
            deadline_ = now + 20'000;
            return WifiAction::Retry;
        }
        const bool stopping = state_ == WifiState::Starting || state_ == WifiState::Stopping;
        schedule(now, jitter);
        return stopping ? WifiAction::Stop : WifiAction::Disconnect;
    }
    WifiState state() const { return state_; }
    uint32_t retry_delay_ms(int64_t now) const {
        return state_ == WifiState::Backoff && deadline_ > now ?
               static_cast<uint32_t>(deadline_ - now) : 0;
    }
private:
    void schedule(int64_t now, uint32_t jitter) {
        const uint32_t base = backoff_.next_delay_ms();
        const uint32_t extra = jitter % (base / 4 + 1);
        deadline_ = now + (base > 60'000 - extra ? 60'000 : base + extra);
        state_ = WifiState::Backoff;
    }
    WifiState state_{WifiState::Unconfigured};
    Backoff backoff_;
    int64_t deadline_{};
    int64_t online_since_{};
};
}  // namespace goldrop
