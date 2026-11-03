#include "goldrop/tcp_transport.hpp"
#include "fake_dns.hpp"
#include "goldrop/ram_measurement_store.hpp"
#include "goldrop/telemetry_service.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>

namespace {
void require(bool ok, int line) {
    if (!ok) {
        std::fprintf(stderr, "FAIL TCP socket test line %d\n", line);
        std::exit(1);
    }
}
#define REQUIRE(...) require((__VA_ARGS__), __LINE__)
void step(goldrop::TcpTransport &transport, int64_t &now, bool wifi = true) {
    fake_dns::run_tcpip();
    fake_dns::finish();
    transport.poll(now++, wifi);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
}
} // namespace
int main(int argc, char **argv) {
    using namespace goldrop;
    if (argc == 3 && std::string(argv[1]) == "--receiver") {
        ServerConfig endpoint;
        REQUIRE(endpoint.set_address("127.0.0.1", 9));
        REQUIRE(endpoint.set_port(argv[2], std::strlen(argv[2])));
        TcpTransport client;
        RamMeasurementStore<2> queue;
        TelemetryService telemetry(queue, client);
        Measurement sample;
        sample.device_id.mac = {2, 0, 0x0a, 0x10, 0xef, 0xff};
        sample.boot_id = 99;
        sample.sequence = 42;
        sample.sht45_valid = true;
        sample.sht45_error = SensorError::None;
        sample.temperature_sht_c = 23.5F;
        sample.humidity_rh = 40;
        REQUIRE(telemetry.ingest(sample) == AppendResult::Stored);
        REQUIRE(client.configure_server(endpoint, 0));
        int64_t tick = 0;
        bool wrote = false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            client.poll(tick++, true);
            Measurement receipt;
            if (client.take_written(receipt)) {
                wrote = true;
                REQUIRE(queue.size() == 1);
            }
            if (client.take_acknowledged(receipt))
                REQUIRE(telemetry.acknowledge(receipt));
            if (!queue.size()) {
                REQUIRE(wrote && client.backend_ready());
                return 0;
            }
            telemetry.poll(tick);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        REQUIRE(false);
    }
    const int listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    REQUIRE(listener >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(listener, reinterpret_cast<const sockaddr *>(&address), sizeof(address)) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(getsockname(listener, reinterpret_cast<sockaddr *>(&address), &length) == 0);
    REQUIRE(listen(listener, 2) == 0);
    REQUIRE(fcntl(listener, F_SETFL, O_NONBLOCK) == 0);
    ServerConfig config;
    REQUIRE(config.set_address("r.limanowa.org", 14));
    config.port = ntohs(address.sin_port);
    TcpTransport transport;
    int64_t now = 0;
    REQUIRE(transport.configure_server(config, now));
    step(transport, now, false);
    REQUIRE(transport.server_state() == ServerState::WaitingWifi);
    for (unsigned i = 0; i < 100 && !transport.connected(); ++i)
        step(transport, now);
    REQUIRE(transport.connected());
    REQUIRE(fake_dns::last_host() == "r.limanowa.org");
    int peer = accept(listener, nullptr, nullptr);
    REQUIRE(peer >= 0 && fcntl(peer, F_SETFL, O_NONBLOCK) == 0);
    Measurement measurement{};
    measurement.sequence = 42;
    measurement.boot_id = 99;
    measurement.device_id.mac = {0x02, 0x00, 0x0a, 0x10, 0xef, 0xff};
    measurement.sht45_valid = true;
    measurement.sht45_error = SensorError::None;
    measurement.temperature_sht_c = 23.5F;
    measurement.humidity_rh = 40;
    REQUIRE(transport.try_send(measurement) == SendResult::Accepted);
    REQUIRE(transport.try_send(measurement) == SendResult::Busy);
    Measurement written;
    REQUIRE(!transport.take_written(written));
    bool receipt = false;
    std::string line;
    for (unsigned i = 0; i < 100 && (!receipt || line.find('\n') == std::string::npos); ++i) {
        step(transport, now);
        if (transport.take_written(written))
            receipt = true;
        char bytes[1024];
        const auto received = recv(peer, bytes, sizeof(bytes), 0);
        if (received > 0)
            line.append(bytes, static_cast<std::size_t>(received));
    }
    REQUIRE(receipt && same_measurement(written, measurement));
    REQUIRE(line.front() == '{' && line.back() == '\n');
    REQUIRE(line.find("\"sequence\":\"42\"") != std::string::npos);
    REQUIRE(line.find("\"device_id\":\"02:00:0a:10:ef:ff\"") != std::string::npos);
    REQUIRE(line.find("\"temperature_sht_c\":23.500") != std::string::npos);
    REQUIRE(!transport.take_written(written));
    REQUIRE(!transport.backend_ready());
    Measurement confirmed;
    const std::string wrong_ack = "ACK1 02:00:0a:10:ef:ff 99 43\n";
    REQUIRE(send(peer, wrong_ack.data(), wrong_ack.size(), 0) == static_cast<ssize_t>(wrong_ack.size()));
    for (unsigned i = 0; i < 10; ++i)
        step(transport, now);
    REQUIRE(!transport.take_acknowledged(confirmed) && !transport.backend_ready());
    const std::string partial = "ACK1 02:00:0a:10:ef:ff 99 ";
    REQUIRE(send(peer, partial.data(), partial.size(), 0) == static_cast<ssize_t>(partial.size()));
    for (unsigned i = 0; i < 10; ++i)
        step(transport, now);
    REQUIRE(!transport.take_acknowledged(confirmed));
    REQUIRE(send(peer, "42\n", 3, 0) == 3);
    bool acknowledged = false;
    for (unsigned i = 0; i < 100 && !acknowledged; ++i) {
        step(transport, now);
        acknowledged = transport.take_acknowledged(confirmed);
    }
    REQUIRE(acknowledged && same_measurement(confirmed, measurement) && transport.backend_ready());
    const std::string duplicate = "ACK1 02:00:0a:10:ef:ff 99 42\n";
    REQUIRE(send(peer, duplicate.data(), duplicate.size(), 0) == static_cast<ssize_t>(duplicate.size()));
    for (unsigned i = 0; i < 10; ++i)
        step(transport, now);
    REQUIRE(!transport.take_acknowledged(confirmed));
    // Wi-Fi loss after enqueue, before write, must not generate a receipt.
    REQUIRE(transport.try_send_status("{\"message_type\":\"status\",\"firmware_version\":\"0.2.0\"}"));
    std::string status_line;
    for (unsigned i = 0; i < 100 && status_line.find('\n') == std::string::npos; ++i) {
        step(transport, now);
        REQUIRE(!transport.take_written(written));
        char bytes[512];
        const auto received = recv(peer, bytes, sizeof(bytes), 0);
        if (received > 0)
            status_line.append(bytes, static_cast<std::size_t>(received));
    }
    REQUIRE(status_line.find("\"message_type\":\"status\"") != std::string::npos);
    ++measurement.sequence;
    REQUIRE(transport.try_send(measurement) == SendResult::Accepted);
    step(transport, now, false);
    REQUIRE(!transport.take_written(written));
    REQUIRE(transport.server_state() == ServerState::WaitingWifi);
    REQUIRE(close(peer) == 0);
    for (unsigned i = 0; i < 100 && !transport.connected(); ++i)
        step(transport, now);
    REQUIRE(transport.connected());
    peer = accept(listener, nullptr, nullptr);
    REQUIRE(peer >= 0);
    REQUIRE(close(peer) == 0);
    for (unsigned i = 0; i < 100 && transport.connected(); ++i)
        step(transport, now);
    REQUIRE(transport.server_state() == ServerState::Backoff);
    REQUIRE(!transport.take_written(written));
    now += 1001;
    for (unsigned i = 0; i < 100 && !transport.connected(); ++i)
        step(transport, now);
    REQUIRE(transport.connected());
    peer = accept(listener, nullptr, nullptr);
    REQUIRE(peer >= 0);
    REQUIRE(transport.try_send(measurement) == SendResult::Accepted);
    receipt = false;
    for (unsigned i = 0; i < 100 && !receipt; ++i) {
        step(transport, now);
        receipt = transport.take_written(written);
    }
    REQUIRE(receipt && !transport.take_acknowledged(confirmed));
    now += settings::kAckTimeoutMs;
    step(transport, now);
    REQUIRE(transport.server_state() == ServerState::Backoff && !transport.backend_ready());
    close(peer);
    transport.disable_server();
    REQUIRE(transport.server_state() == ServerState::Unconfigured);
    // Refused connection schedules retry and returns immediately.
    REQUIRE(close(listener) == 0);
    REQUIRE(transport.configure_server(config, now));
    for (unsigned i = 0; i < 100 && transport.server_state() != ServerState::Backoff; ++i)
        step(transport, now);
    REQUIRE(transport.server_state() == ServerState::Backoff);
    std::puts("PASS: real host TCP sockets, NDJSON, receipts, Wi-Fi loss, peer close, refused connection");
}
