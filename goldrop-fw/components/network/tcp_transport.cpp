#include "goldrop/tcp_transport.hpp"
#include "esp_log.h"
#include "esp_random.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace goldrop {
namespace {
constexpr const char *kTag = "server";
}
TcpTransport::~TcpTransport() { close_socket(); }
bool TcpTransport::configure_server(const ServerConfig &config, int64_t now) {
    if (!config.valid())
        return false;
    close_socket();
    config_ = config;
    now_ms_ = now;
    retry_.configure();
    ESP_LOGI(kTag, "TCP configured: %s:%u; backend ACK required", config.host.data(), config.port);
    return true;
}
void TcpTransport::disable_server() {
    close_socket();
    config_ = {};
    retry_.disable();
}
void TcpTransport::close_socket() {
    dns_.cancel();
    resolved_ipv4_ = 0;
    if (socket_ >= 0 && close(socket_) != 0)
        ESP_LOGW(kTag, "Socket close failed, errno=%d", errno);
    socket_ = -1;
    // A partial frame is retried as a whole on a new connection; no receipt is emitted.
    pending_.reset();
    status_frame_ = false;
    waiting_ack_ = false;
    backend_ready_ = false;
    written_ready_ = false;
    ack_reader_.reset();
}
void TcpTransport::fail(int64_t now, const char *operation, int error) {
    close_socket();
    retry_.failed(now, esp_random());
    ESP_LOGW(kTag, "%s failed, errno=%d; reconnect scheduled", operation, error);
}
void TcpTransport::mark_connected(int64_t now) {
    retry_.connected(now);
    const int enabled = 1, idle = settings::kTcpKeepaliveIdleSeconds,
              interval = settings::kTcpKeepaliveIntervalSeconds, count = settings::kTcpKeepaliveProbeCount;
    if (setsockopt(socket_, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled)) != 0 ||
        setsockopt(socket_, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle)) != 0 ||
        setsockopt(socket_, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval)) != 0 ||
        setsockopt(socket_, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count)) != 0) {
        ESP_LOGW(kTag, "Keepalive setup failed, errno=%d", errno);
    }
    ESP_LOGI(kTag, "TCP connected; waiting for durable backend receipts");
}
void TcpTransport::begin_connection(int64_t now) {
    std::array<uint8_t, 4> address{};
    if (ServerConfig::parse_ipv4(config_.host.data(), address)) {
        resolved_ipv4_ = htonl((uint32_t(address[0]) << 24) | (uint32_t(address[1]) << 16) |
                               (uint32_t(address[2]) << 8) | address[3]);
        connect_socket(now);
        return;
    }
    retry_.resolving(now);
    if (!dns_.start(config_.host.data())) {
        fail(now, "DNS request", EAGAIN);
        return;
    }
    ESP_LOGI(kTag, "Resolving DNS: %s", config_.host.data());
}
void TcpTransport::connect_socket(int64_t now) {
    socket_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_ < 0) {
        fail(now, "Socket creation", errno);
        return;
    }
    const int flags = fcntl(socket_, F_GETFL, 0);
    if (flags < 0 || fcntl(socket_, F_SETFL, flags | O_NONBLOCK) != 0) {
        fail(now, "Nonblocking setup", errno);
        return;
    }
    sockaddr_in endpoint{};
    endpoint.sin_family = AF_INET;
    endpoint.sin_port = htons(config_.port);
    endpoint.sin_addr.s_addr = resolved_ipv4_;
    if (connect(socket_, reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint)) == 0) {
        mark_connected(now);
    } else if (errno != EINPROGRESS && errno != EWOULDBLOCK) {
        fail(now, "Connect", errno);
    }
}
SendResult TcpTransport::try_send(const Measurement &measurement) {
    if (!connected())
        return SendResult::Offline;
    if (pending_.busy() || waiting_ack_ || written_ready_ || ack_ready_)
        return SendResult::Busy;
    if (!pending_.prepare(measurement))
        return SendResult::Failed;
    write_started_ms_ = now_ms_;
    status_frame_ = false;
    expected_ = measurement;
    return SendResult::Accepted;
}
bool TcpTransport::take_written(Measurement &measurement) {
    if (!written_ready_)
        return false;
    measurement = expected_;
    written_ready_ = false;
    return true;
}
bool TcpTransport::take_acknowledged(Measurement &measurement) {
    if (!ack_ready_)
        return false;
    measurement = acknowledged_;
    ack_ready_ = false;
    return true;
}
bool TcpTransport::try_send_status(std::string_view line) {
    if (!connected() || !pending_.prepare_status(line))
        return false;
    write_started_ms_ = now_ms_;
    status_frame_ = true;
    return true;
}
void TcpTransport::poll(int64_t now, bool wifi) {
    now_ms_ = now;
    const TcpAction action = retry_.poll(now, wifi, esp_random());
    if (action == TcpAction::Close) {
        close_socket();
        ESP_LOGW(kTag, "Request closed: Wi-Fi unavailable or DNS/connect deadline exceeded");
    } else if (action == TcpAction::Connect) {
        begin_connection(now);
    }
    if (server_state() == ServerState::Resolving && dns_.take_result(resolved_ipv4_)) {
        if (!resolved_ipv4_) {
            fail(now, "DNS lookup", EHOSTUNREACH);
            return;
        }
        const auto address = ntohl(resolved_ipv4_);
        ESP_LOGI(kTag, "DNS resolved: %s -> %u.%u.%u.%u", config_.host.data(), unsigned(address >> 24),
                 unsigned((address >> 16) & 255), unsigned((address >> 8) & 255), unsigned(address & 255));
        retry_.resolved(now);
        connect_socket(now);
    }
    if (server_state() == ServerState::Connecting && socket_ >= 0) {
        fd_set writable, errors;
        FD_ZERO(&writable);
        FD_ZERO(&errors);
        FD_SET(socket_, &writable);
        FD_SET(socket_, &errors);
        timeval timeout{};
        const int ready = select(socket_ + 1, nullptr, &writable, &errors, &timeout);
        if (ready < 0) {
            if (errno != EINTR)
                fail(now, "Connect readiness", errno);
            return;
        }
        if (ready > 0) {
            int error = 0;
            socklen_t length = sizeof(error);
            if (getsockopt(socket_, SOL_SOCKET, SO_ERROR, &error, &length) != 0) {
                fail(now, "Connect status", errno);
                return;
            }
            if (error) {
                fail(now, "Connect", error);
                return;
            }
            mark_connected(now);
        }
    }
    if (!connected())
        return;
    if (waiting_ack_ && now >= ack_deadline_) {
        fail(now, "Backend ACK deadline", ETIMEDOUT);
        return;
    }
    ack_reader_.poll(now);
    char bytes[128];
    const int received = recv(socket_, bytes, sizeof(bytes), 0);
    if (received == 0) {
        fail(now, "Peer closed", 0);
        return;
    }
    if (received < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
        fail(now, "Read", errno);
        return;
    }
    for (int i = 0; i < received; ++i) {
        Measurement ack;
        if (ack_reader_.feed(bytes[i], now, ack) && waiting_ack_ && same_measurement(ack, expected_)) {
            acknowledged_ = expected_;
            ack_ready_ = true;
            waiting_ack_ = false;
            backend_ready_ = true;
        }
    }
    if (pending_.busy() && pending_.remaining()) {
        if (now - write_started_ms_ >= settings::kTcpWriteTimeoutMs) {
            fail(now, "Write deadline", ETIMEDOUT);
            return;
        }
        const int written = send(socket_, pending_.data(), pending_.remaining(), 0);
        if (written > 0) {
            if (!pending_.advance(static_cast<std::size_t>(written)))
                fail(now, "Write count", EIO);
        } else if (written == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            fail(now, "Write", written == 0 ? EPIPE : errno);
        }
    }
    if (pending_.busy() && pending_.remaining() == 0) {
        if (!status_frame_) {
            written_ready_ = true;
            waiting_ack_ = true;
            ack_deadline_ = now + settings::kAckTimeoutMs;
        }
        pending_.reset();
        status_frame_ = false;
    }
}
} // namespace goldrop
