#pragma once
#include "goldrop/server_config.hpp"

namespace goldrop {
enum class ServerState { Unconfigured, WaitingWifi, Resolving, Connecting, Connected, Backoff };
inline const char *server_state_name(ServerState state) {
    switch (state) {
    case ServerState::Unconfigured:
        return "unconfigured";
    case ServerState::WaitingWifi:
        return "waiting_wifi";
    case ServerState::Resolving:
        return "resolving_dns";
    case ServerState::Connecting:
        return "connecting";
    case ServerState::Connected:
        return "connected";
    case ServerState::Backoff:
        return "backoff";
    }
    return "unknown";
}
class ServerControl {
  public:
    virtual ~ServerControl() = default;
    virtual bool configure_server(const ServerConfig &config, int64_t now_ms) = 0;
    virtual void disable_server() = 0;
    virtual ServerState server_state() const = 0;
    virtual bool backend_ready() const { return server_state() == ServerState::Connected; }
};
} // namespace goldrop
