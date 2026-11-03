#pragma once
#include "goldrop/server_control.hpp"
#include "goldrop/backoff.hpp"
#include "goldrop/settings.hpp"

namespace goldrop {
enum class TcpAction { None, Connect, Close };
class TcpRetryPolicy {
public:
    void configure() { backoff_.reset(); state_ = ServerState::WaitingWifi; }
    void disable() { state_ = ServerState::Unconfigured; backoff_.reset(); }
    void resolving(int64_t now) {
        if (state_ == ServerState::Connecting) { state_ = ServerState::Resolving; deadline_ = now + settings::kDnsTimeoutMs; }
    }
    void resolved(int64_t now) {
        if (state_ == ServerState::Resolving) { state_ = ServerState::Connecting; deadline_ = now + settings::kTcpConnectTimeoutMs; }
    }
    void connected(int64_t now) {
        if (state_ == ServerState::Connecting) { state_ = ServerState::Connected; online_since_ = now; }
    }
    void failed(int64_t now, uint32_t jitter = 0) {
        if (state_ != ServerState::Resolving && state_ != ServerState::Connecting && state_ != ServerState::Connected) return;
        if (state_ == ServerState::Connected && now - online_since_ >= settings::kTcpStableConnectionMs) backoff_.reset();
        const uint32_t base = backoff_.next_delay_ms();
        const uint32_t extra = jitter % (base / 4 + 1);
        deadline_ = now + (base > 60'000 - extra ? 60'000 : base + extra);
        state_ = ServerState::Backoff;
    }
    TcpAction poll(int64_t now, bool wifi, uint32_t jitter = 0) {
        if (state_ == ServerState::Unconfigured) return TcpAction::None;
        if (!wifi) {
            const bool active = state_ == ServerState::Resolving || state_ == ServerState::Connecting || state_ == ServerState::Connected;
            state_ = ServerState::WaitingWifi;
            return active ? TcpAction::Close : TcpAction::None;
        }
        if (state_ == ServerState::Connected) {
            if (now - online_since_ >= settings::kTcpStableConnectionMs) backoff_.reset();
            return TcpAction::None;
        }
        if (state_ == ServerState::Resolving || state_ == ServerState::Connecting) {
            if (now < deadline_) return TcpAction::None;
            failed(now, jitter);
            return TcpAction::Close;
        }
        if (state_ == ServerState::Backoff && now < deadline_) return TcpAction::None;
        state_ = ServerState::Connecting;
        deadline_ = now + settings::kTcpConnectTimeoutMs;
        return TcpAction::Connect;
    }
    ServerState state() const { return state_; }
private:
    ServerState state_{ServerState::Unconfigured};
    Backoff backoff_;
    int64_t deadline_{}, online_since_{};
};
}  // namespace goldrop
