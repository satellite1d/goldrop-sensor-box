#pragma once
#include "goldrop/server_control.hpp"
#include "goldrop/tcp_retry_policy.hpp"
#include "goldrop/tcp_write_buffer.hpp"
#include "goldrop/telemetry_transport.hpp"
#include "goldrop/async_dns.hpp"
#include "goldrop/backend_ack.hpp"

namespace goldrop {
// Owned and polled only by telemetry_task. All socket operations are nonblocking.
class TcpTransport final : public TelemetryTransport, public ServerControl {
  public:
    ~TcpTransport() override;
    bool configure_server(const ServerConfig &config, int64_t now_ms) override;
    void disable_server() override;
    ServerState server_state() const override { return retry_.state(); }
    bool connected() const override { return server_state() == ServerState::Connected; }
    bool backend_ready() const override { return connected() && backend_ready_; }
    SendResult try_send(const Measurement &measurement) override;
    bool try_send_status(std::string_view line);
    void poll(int64_t now_ms, bool wifi_ready);
    bool take_written(Measurement &measurement);
    bool take_acknowledged(Measurement &measurement);

  private:
    void close_socket();
    void fail(int64_t now_ms, const char *operation, int error);
    void begin_connection(int64_t now_ms);
    void connect_socket(int64_t now_ms);
    void mark_connected(int64_t now_ms);
    ServerConfig config_;
    TcpRetryPolicy retry_;
    TcpWriteBuffer pending_;
    AsyncDns dns_;
    uint32_t resolved_ipv4_{};
    bool status_frame_{};
    BackendAckReader ack_reader_;
    Measurement expected_, acknowledged_;
    bool waiting_ack_{}, written_ready_{}, ack_ready_{}, backend_ready_{};
    int64_t ack_deadline_{};
    int socket_{-1};
    int64_t now_ms_{}, write_started_ms_{};
};
} // namespace goldrop
