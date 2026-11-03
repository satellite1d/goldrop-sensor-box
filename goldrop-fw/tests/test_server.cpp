#include "goldrop/server_config_store.hpp"
#include "goldrop/tcp_retry_policy.hpp"
#include "goldrop/tcp_write_buffer.hpp"
#include "goldrop/wifi_provisioner.hpp"
#include "goldrop/ram_measurement_store.hpp"
#include "goldrop/telemetry_service.hpp"
#include "goldrop/backend_ack.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
using namespace goldrop;
void require(bool ok, int line) { if (!ok) { std::fprintf(stderr, "FAIL server test line %d\n", line); std::exit(1); } }
#define REQUIRE(...) require((__VA_ARGS__), __LINE__)
class Store final : public WifiConfigStore, public ServerConfigStore {
public:
    bool writable{true}; unsigned wifi_writes{}, server_writes{};
    ServerConfig saved;
    ConfigLoadResult load_wifi(WifiCredentials&) override { return ConfigLoadResult::Defaults; }
    bool save_wifi(const WifiCredentials&) override { ++wifi_writes; return true; }
    bool forget_wifi() override { return true; }
    ConfigLoadResult load_server(ServerConfig& output) override { output = saved; return ConfigLoadResult::Loaded; }
    bool save_server(const ServerConfig& config) override {
        if (!writable) return false;
        saved = config; ++server_writes; return true;
    }
    bool forget_server() override { if (!writable) return false; saved = {}; return true; }
};
class Control final : public NetworkControl, public ServerControl, public TelemetryTransport {
public:
    unsigned configurations{}, disables{}, sends{};
    bool configure(const WifiCredentials&, int64_t) override { return true; }
    void disable(int64_t) override {}
    NetworkStatus status() const override { return {}; }
    bool configure_server(const ServerConfig&, int64_t) override { ++configurations; return true; }
    void disable_server() override { ++disables; }
    ServerState server_state() const override { return ServerState::Unconfigured; }
    bool connected() const override { return true; }
    SendResult try_send(const Measurement&) override { ++sends; return SendResult::Accepted; }
};
ProvisionReply send(WifiProvisioner& protocol, const std::string& line, int64_t now = 0) {
    ProvisionReply result = ProvisionReply::None;
    for (char c : line) { const auto reply = protocol.feed(c, now++); if (reply != ProvisionReply::None) result = reply; }
    return result;
}
void test_endpoint_and_codec() {
    ServerConfig config;
    REQUIRE(config.set_address("192.168.1.50", 12));
    REQUIRE(config.set_port("65535", 5));
    REQUIRE(config.valid());
    for (const std::string value : {"", "https://host.example", "1.2.3", "1.2.3.256", "1..2.3", "0.0.0.0", "224.0.0.1", "01.2.3.4", "bad_name.org", "-bad.org", "bad-.org", "host:9000", "host/name", "host.", "user@host"})
        REQUIRE(!config.set_address(value.data(), value.size()));
    for (const std::string value : {"0", "65536", "-1", "80x", " 80", ""})
        REQUIRE(!config.set_port(value.data(), value.size()));
    ServerConfigBlob blob;
    REQUIRE(encode_server_config(config, blob));
    REQUIRE(blob[6] == 255 && blob[7] == 255);
    ServerConfig restored;
    REQUIRE(decode_server_config(blob.data(), blob.size(), restored));
    REQUIRE(restored.host == config.host && restored.port == 65535);
    for (unsigned offset : {0u, 4u, 5u, 8u}) {
        auto corrupt = blob; corrupt[offset] = 255;
        REQUIRE(!decode_server_config(corrupt.data(), corrupt.size(), restored));
        REQUIRE(!restored.valid());
    }
    REQUIRE(!decode_server_config(blob.data(), 11, restored));
    REQUIRE(config.set_address("R.LIMANOWA.ORG", 14));
    REQUIRE(std::string(config.host.data()) == "r.limanowa.org");
    REQUIRE(encode_server_config(config, blob) && blob[4] == 2 && blob[5] == 14);
    REQUIRE(decode_server_config(blob.data(), blob.size(), restored) && restored.host == config.host);
    auto corrupt = blob; corrupt[8] = 0;
    REQUIRE(!decode_server_config(corrupt.data(), corrupt.size(), restored));
    corrupt = blob; corrupt.back() = 1;
    REQUIRE(!decode_server_config(corrupt.data(), corrupt.size(), restored));
    const std::string longest = std::string(63, 'a') + "." + std::string(63, 'b') + "." + std::string(63, 'c') + "." + std::string(61, 'd');
    REQUIRE(longest.size() == 253 && config.set_address(longest.data(), longest.size()));
    REQUIRE(encode_server_config(config, blob) && decode_server_config(blob.data(), blob.size(), restored));
    REQUIRE(!config.set_address((longest + "x").data(), longest.size() + 1));
    const std::string long_label(64, 'a');
    REQUIRE(!config.set_address(long_label.data(), long_label.size()));
    LegacyServerConfigBlob legacy{'G', 'T', 'C', 'P', 1, 0, 0x23, 0x28, 192, 168, 1, 50};
    REQUIRE(decode_server_config(legacy.data(), legacy.size(), restored));
    REQUIRE(std::string(restored.host.data()) == "192.168.1.50" && restored.port == 9000);
    legacy[4] = 2;
    REQUIRE(!decode_server_config(legacy.data(), legacy.size(), restored));
    config.host.fill('x');
    REQUIRE(!config.valid());
}
void test_server_provisioning() {
    Store store; Control control;
    WifiProvisioner protocol(store, control, &store, &control);
    REQUIRE(send(protocol, "server save\n") == ProvisionReply::Invalid);
    REQUIRE(send(protocol, "server address r.limanowa.org\n") == ProvisionReply::ServerAddress);
    REQUIRE(send(protocol, "server port 9000\n") == ProvisionReply::ServerPort);
    REQUIRE(store.server_writes == 0 && control.configurations == 0);
    REQUIRE(send(protocol, "server port 65536\n") == ProvisionReply::Invalid);
    REQUIRE(send(protocol, "server save\n") == ProvisionReply::Invalid);
    REQUIRE(send(protocol, "server port 9000\n") == ProvisionReply::ServerPort);
    store.writable = false;
    REQUIRE(send(protocol, "server save\n") == ProvisionReply::StorageError);
    REQUIRE(control.configurations == 0);
    store.writable = true;
    REQUIRE(send(protocol, "server save\n") == ProvisionReply::ServerSaved);
    REQUIRE(store.saved.port == 9000 && store.server_writes == 1 && store.wifi_writes == 0);
    REQUIRE(std::string(store.saved.host.data()) == "r.limanowa.org");
    REQUIRE(send(protocol, "server status\n") == ProvisionReply::ServerStatus);
    REQUIRE(send(protocol, "server address 10.0.0.1\n", 1000) == ProvisionReply::ServerAddress);
    REQUIRE(protocol.tick(302000) == ProvisionReply::Timeout);
    REQUIRE(send(protocol, "server port 1234\n", 302001) == ProvisionReply::ServerPort);
    REQUIRE(send(protocol, "server save\n", 302050) == ProvisionReply::Invalid);
    protocol.cancel();
    REQUIRE(send(protocol, "server save\n") == ProvisionReply::Invalid);
    store.writable = false;
    REQUIRE(send(protocol, "server forget\n") == ProvisionReply::StorageError && control.disables == 0);
    store.writable = true;
    REQUIRE(send(protocol, "server forget\n") == ProvisionReply::ServerForgotten && control.disables == 1);
    const std::string longest = std::string(63, 'a') + "." + std::string(63, 'b') + "." + std::string(63, 'c') + "." + std::string(61, 'd');
    REQUIRE(send(protocol, "server address " + longest + "\n") == ProvisionReply::ServerAddress);
    REQUIRE(send(protocol, "server port 9000\n") == ProvisionReply::ServerPort);
    REQUIRE(send(protocol, "server save\n") == ProvisionReply::ServerSaved);
    REQUIRE(std::string(store.saved.host.data()) == longest);
}
void test_retry_and_frame_receipts() {
    TcpRetryPolicy retry;
    REQUIRE(retry.poll(0, true) == TcpAction::None);
    retry.configure();
    REQUIRE(retry.poll(0, false) == TcpAction::None);
    REQUIRE(retry.state() == ServerState::WaitingWifi);
    TcpRetryPolicy dns_retry;
    dns_retry.configure();
    REQUIRE(dns_retry.poll(0, true) == TcpAction::Connect);
    dns_retry.resolving(0);
    REQUIRE(dns_retry.state() == ServerState::Resolving);
    REQUIRE(dns_retry.poll(9999, true) == TcpAction::None);
    REQUIRE(dns_retry.poll(10000, true) == TcpAction::Close);
    REQUIRE(dns_retry.poll(11000, true) == TcpAction::Connect);
    dns_retry.resolving(11000); dns_retry.resolved(20000);
    REQUIRE(dns_retry.state() == ServerState::Connecting);
    REQUIRE(dns_retry.poll(24999, true) == TcpAction::None);
    REQUIRE(dns_retry.poll(25000, true) == TcpAction::Close);
    REQUIRE(retry.poll(0, true) == TcpAction::Connect);
    REQUIRE(retry.poll(4999, true) == TcpAction::None);
    REQUIRE(retry.poll(5000, true) == TcpAction::Close);
    REQUIRE(retry.poll(5999, true) == TcpAction::None);
    REQUIRE(retry.poll(6000, true) == TcpAction::Connect);
    retry.connected(6000);
    retry.failed(6001);
    REQUIRE(retry.poll(8000, true) == TcpAction::None);
    REQUIRE(retry.poll(8001, true) == TcpAction::Connect);
    retry.connected(8001);
    REQUIRE(retry.poll(38001, true) == TcpAction::None);
    retry.failed(38002);
    REQUIRE(retry.poll(39002, true) == TcpAction::Connect);
    REQUIRE(retry.poll(39003, false) == TcpAction::Close);
    REQUIRE(retry.state() == ServerState::WaitingWifi);
    int64_t now = 40000;
    for (unsigned i = 0; i < 20; ++i) {
        REQUIRE(retry.poll(now, true) == TcpAction::Connect);
        retry.failed(now, 0xffffffff);
        REQUIRE(retry.poll(now + 999, true) == TcpAction::None);
        now += 60000;
    }
    Measurement measurement{}; measurement.sequence = 4; measurement.boot_id = 10;
    TcpWriteBuffer frame;
    REQUIRE(frame.prepare(measurement));
    const std::string line(frame.data(), frame.remaining());
    REQUIRE(line.back() == '\n' && line.front() == '{');
    REQUIRE(line.find("\"sequence\":\"4\"") != std::string::npos);
    REQUIRE(!frame.prepare(measurement));
    Measurement receipt;
    REQUIRE(!frame.take_written(receipt));
    REQUIRE(frame.advance(3));
    REQUIRE(!frame.take_written(receipt));
    REQUIRE(!frame.advance(frame.remaining() + 1));
    REQUIRE(frame.advance(frame.remaining()));
    REQUIRE(frame.take_written(receipt) && same_measurement(measurement, receipt));
    REQUIRE(!frame.take_written(receipt));
    REQUIRE(frame.prepare(measurement));
    REQUIRE(frame.advance(1)); frame.reset();  // Socket closed during a partial write.
    REQUIRE(!frame.take_written(receipt));
    REQUIRE(frame.prepare(measurement) && std::string(frame.data(), frame.remaining()) == line);
}
void test_confirmation_policy() {
    Control transport;
    RamMeasurementStore<2> store;
    Measurement measurement{}; measurement.sequence = 4; measurement.boot_id = 10;
    TelemetryService backend(store, transport);
    backend.ingest(measurement); backend.poll(0);
    REQUIRE(!backend.acknowledge(measurement, DeliveryConfirmation::TcpWrite));
    REQUIRE(store.size() == 1);
    REQUIRE(backend.acknowledge(measurement));
    TelemetryService tcp(store, transport, 30000, DeliveryConfirmation::TcpWrite);
    tcp.ingest(measurement); tcp.poll(0);
    REQUIRE(store.size() == 1);  // Accepted/enqueued does not acknowledge a frame.
    REQUIRE(!tcp.acknowledge(measurement));
    tcp.poll(29999); REQUIRE(transport.sends == 2);
    tcp.poll(30000); REQUIRE(transport.sends == 3);
    Measurement wrong = measurement; ++wrong.sequence;
    REQUIRE(!tcp.acknowledge(wrong, DeliveryConfirmation::TcpWrite));
    REQUIRE(tcp.acknowledge(measurement, DeliveryConfirmation::TcpWrite) && store.size() == 0);
}
void test_backend_ack_protocol(){
    Measurement ack;
    REQUIRE(parse_backend_ack("ACK1 02:00:00:00:00:01 0 1",ack));
    REQUIRE(ack.boot_id==0&&ack.sequence==1);
    REQUIRE(parse_backend_ack("ACK1 02:00:00:00:00:01 18446744073709551615 18446744073709551615",ack));
    REQUIRE(ack.boot_id==UINT64_MAX&&ack.sequence==UINT64_MAX);
    for(const auto *bad:{"","ACK1 02:00:00:00:00:01 0 0","ACK1 02:00:00:00:00:01 -1 1","ACK1 02:00:00:00:00:01 01 1","ACK1 02:00:00:00:00:01 1 18446744073709551616","ACK1 03:00:00:00:00:01 1 1","ACK1 02:00:00:00:00:01 1 1 extra","ACK1 02:00:00:00:00:01 1 1\n"})
        REQUIRE(!parse_backend_ack(bad,ack));
    BackendAckReader reader;
    const std::string line="ACK1 02:00:00:00:00:01 10 12\r\n";
    for(std::size_t i=0;i<line.size();++i)REQUIRE(reader.feed(line[i],0,ack)==(i+1==line.size()));
    REQUIRE(ack.boot_id==10&&ack.sequence==12);
    for(unsigned i=0;i<1000;++i)REQUIRE(!reader.feed('x',1,ack));
    REQUIRE(!reader.feed('\n',1,ack));
    for(char c:line)reader.feed(c,2,ack);
    REQUIRE(ack.sequence==12);
    REQUIRE(!reader.feed('A',0,ack));reader.poll(5000);
    for(char c:line)REQUIRE(!reader.feed(c,5001,ack));
    bool recovered=false;for(char c:line)recovered=reader.feed(c,5002,ack)||recovered;
    REQUIRE(recovered);
}
}
void test_server_transport_and_configuration() {
    test_endpoint_and_codec(); test_server_provisioning();
    test_retry_and_frame_receipts(); test_confirmation_policy();
    test_backend_ack_protocol();
}
