#include "goldrop/config_manager.hpp"
#include "goldrop/management_protocol.hpp"
#include "goldrop/binary_codec.hpp"
#include "goldrop/wifi_provisioner.hpp"
#include <cassert>
#include <iostream>
#include <iterator>
using namespace goldrop;
namespace {
struct Store : ManagedConfigStore {
    ManagedConfigBlob saved{};
    bool exists{}, fail{}, corrupt{};
    unsigned writes{};
    ConfigLoadResult load(ManagedConfigState &s) override {
        if (corrupt)
            return ConfigLoadResult::Failed;
        if (!exists)
            return ConfigLoadResult::Defaults;
        return decode_managed_config(saved.data(), saved.size(), s) ? ConfigLoadResult::Loaded
                                                                    : ConfigLoadResult::Failed;
    }
    bool save(const ManagedConfigState &s) override {
        if (fail)
            return false;
        ++writes;
        exists = encode_managed_config(s, saved);
        return exists;
    }
};
struct Wifi : NetworkControl {
    NetworkStatus current;
    WifiCredentials applied;
    unsigned changes{};
    bool fail{};
    bool configure(const WifiCredentials &c, int64_t) override {
        ++changes;
        applied = c;
        current.state = WifiState::Connecting;
        return !fail;
    }
    void disable(int64_t) override {
        current.state = WifiState::Unconfigured;
        applied.clear();
        ++changes;
    }
    NetworkStatus status() const override { return current; }
};
struct Server : ServerControl {
    bool ready{true};
    ServerState current{ServerState::Unconfigured};
    unsigned changes{};
    ServerConfig applied;
    bool configure_server(const ServerConfig &c, int64_t) override {
        ++changes;
        applied = c;
        current = ServerState::Connecting;
        return true;
    }
    void disable_server() override {
        current = ServerState::Unconfigured;
        applied = {};
        ++changes;
    }
    ServerState server_state() const override { return current; }
    bool backend_ready() const override { return ready && current == ServerState::Connected; }
};
DeviceId id{{0x90, 0xda, 0x72, 0x9b, 0xcd, 0x4c}};
ConfigBundle bundle(const char *ssid = "old-network") {
    ConfigBundle b;
    assert(b.wifi.set_ssid(ssid, std::strlen(ssid)));
    assert(b.wifi.set_password("test-password", 13));
    assert(b.server.set_address("r.limanowa.org", 14));
    b.server.port = 54321;
    b.present = 3;
    assert(b.valid());
    return b;
}
ConfigCommand command(ConfigAction action, uint64_t rev, uint64_t base = 0, uint64_t target = 0) {
    ConfigCommand c;
    c.action = action;
    c.revision = rev;
    c.base_revision = base;
    c.target_revision = target;
    c.operation[0] = uint8_t(rev);
    if (action == ConfigAction::Apply || action == ConfigAction::Stage)
        c.patch = bundle("new-network");
    return c;
}
struct RuntimeControl : DeviceRuntimeControl {
    uint32_t interval{10000};
    unsigned ota_calls{};
    bool fail{};
    bool set_sampling_interval(uint32_t value) override {
        if (fail)
            return false;
        interval = value;
        return true;
    }
    bool request_firmware_check() override {
        ++ota_calls;
        return !fail;
    }
};
void runtime_commands() {
    Store store;
    Wifi wifi;
    Server server;
    RuntimeControl runtime;
    ConfigManager manager(id, store, wifi, server, &runtime);
    assert(manager.begin(bundle(), true) && manager.apply_active(0));
    auto sampling = command(ConfigAction::Sampling, 1);
    sampling.interval_ms = 60000;
    assert(manager.accept(sampling, 1));
    assert(runtime.interval == 60000 && wifi.changes == 1 && server.changes == 1);
    assert(manager.config_state().result == ConfigResult::Committed);
    assert(manager.config_state().active_revision == 0 && manager.config_state().highwater == 1);
    assert(!manager.accept(sampling, 2));
    assert(manager.accept(command(ConfigAction::OtaCheck, 2), 3) && runtime.ota_calls == 1);
    runtime.fail = true;
    sampling.revision = 3;
    sampling.interval_ms = 30000;
    assert(manager.accept(sampling, 4));
    assert(runtime.interval == 60000 && manager.config_state().result == ConfigResult::Rejected);
    runtime.fail = false;
    assert(manager.accept(command(ConfigAction::Stage, 4), 5));
    sampling.revision = 5;
    assert(manager.accept(sampling, 6));
    assert(runtime.interval == 60000 && manager.config_state().error == ConfigError::Busy);
    ConfigManager reboot(id, store, wifi, server, &runtime);
    assert(reboot.begin({}, true) && reboot.config_state().highwater == 5);
    assert(!reboot.accept(command(ConfigAction::OtaCheck, 2), 7));
    ManagementResponse bytes{};
    std::memcpy(bytes.data(), "GP01", 4);
    bytes[4] = uint8_t(ConfigAction::Sampling);
    binary::put64(bytes.data() + 8, 1);
    bytes[32] = 1;
    ManagementChallenge challenge{};
    binary::put32(bytes.data() + 417, 60000);
    ConfigCommand decoded;
    assert(decode_management_response(bytes, challenge, decoded));
    assert(decoded.action == ConfigAction::Sampling && decoded.interval_ms == 60000);
    binary::put32(bytes.data() + 417, 9999);
    assert(!decode_management_response(bytes, challenge, decoded));
    bytes[4] = uint8_t(ConfigAction::OtaCheck);
    binary::put32(bytes.data() + 417, 0);
    assert(decode_management_response(bytes, challenge, decoded));
}
void online(Wifi &w, Server &s) {
    w.current.state = WifiState::Online;
    s.current = ServerState::Connected;
}
void codec() {
    ManagedConfigState a;
    a.active = bundle();
    a.management.set_endpoint("http://example.org:8000/management/v1/90:da:72:9b:cd:4c");
    a.management.key.fill(0x12);
    a.management.enrolled = a.management.enabled = true;
    ManagedConfigBlob b;
    assert(encode_managed_config(a, b));
    ManagedConfigState decoded;
    assert(decode_managed_config(b.data(), b.size(), decoded));
    assert(decoded.active.wifi.ssid == a.active.wifi.ssid && decoded.management.key == a.management.key);
    for (std::size_t i = 0; i < b.size(); ++i) {
        auto damaged = b;
        damaged[i] ^= 1;
        assert(!decode_managed_config(damaged.data(), damaged.size(), decoded));
    }
    b[90] = 1;
    binary::put32(b.data() + 1276, binary::crc32(b.data(), 1276));
    assert(!decode_managed_config(b.data(), b.size(), decoded));
    assert(!decode_managed_config(b.data(), b.size() - 1, decoded));
}
void transitions() {
    Store store;
    Wifi w;
    Server s;
    ConfigManager m(id, store, w, s);
    assert(m.begin(bundle(), true));
    assert(m.apply_active(0));
    auto c = command(ConfigAction::Apply, 1);
    assert(m.accept(c, 1));
    assert(m.config_state().active.wifi.ssid == bundle().wifi.ssid);
    assert(m.config_state().phase == ConfigPhase::Testing);
    assert(!m.allows_ota(2));
    assert(!m.accept(c, 2));
    auto confirm = command(ConfigAction::Confirm, 1, 0, 1);
    assert(!m.accept(confirm, 3));
    online(w, s);
    s.ready = false;
    assert(!m.accept(confirm, 4));
    s.ready = true;
    assert(m.accept(confirm, 4));
    assert(m.config_state().active_revision == 1 && m.config_state().phase == ConfigPhase::Idle);
    assert(w.applied.ssid == c.patch.wifi.ssid);
    auto next = command(ConfigAction::Apply, 2, 1);
    assert(m.accept(next, 5));
    m.poll(120005, true);
    assert(m.config_state().active_revision == 1 && m.config_state().highwater == 2 &&
           m.config_state().error == ConfigError::Timeout);
    assert(!m.accept(next, 120006));
    assert(m.accept(command(ConfigAction::Apply, 3, 1), 120010));
    Wifi w2;
    Server s2;
    ConfigManager reboot(id, store, w2, s2);
    assert(reboot.begin({}, false));
    assert(reboot.config_state().error == ConfigError::Reboot && reboot.config_state().highwater == 3 &&
           reboot.config_state().active_revision == 1);
    assert(reboot.apply_active(0));
    assert(!reboot.accept(command(ConfigAction::Apply, 3, 1), 1));
    auto wrong = command(ConfigAction::Apply, 4, 0);
    assert(reboot.accept(wrong, 1));
    assert(reboot.config_state().error == ConfigError::BadBase && reboot.config_state().highwater == 4);
}
void staged_and_armed() {
    Store store;
    Wifi w;
    Server s;
    ConfigManager m(id, store, w, s);
    assert(m.begin(bundle(), true));
    assert(m.apply_active(0));
    online(w, s);
    const auto count = w.changes;
    assert(m.accept(command(ConfigAction::Stage, 1), 0));
    assert(w.changes == count);
    assert(m.accept(command(ConfigAction::Arm, 2, 0, 1), 1));
    assert(m.config_state().phase == ConfigPhase::Armed);
    m.poll(500000, true);
    assert(m.config_state().phase == ConfigPhase::Armed);
    w.current.state = WifiState::Backoff;
    m.poll(500001, false);
    m.poll(500002, true);
    m.poll(510002, true);
    assert(m.config_state().phase == ConfigPhase::Testing);
    assert(m.local_override(510003));
    assert(m.config_state().error == ConfigError::LocalOverride && m.config_state().highwater == 2);
    assert(!m.can_poll(510004));
    assert(m.accept(command(ConfigAction::Stage, 3), 550000));
    assert(m.accept(command(ConfigAction::Arm, 4, 0, 3), 550001));
    Wifi w2;
    Server s2;
    ConfigManager reboot(id, store, w2, s2);
    assert(reboot.begin({}, true));
    assert(reboot.apply_active(0));
    reboot.poll(59999, true);
    assert(reboot.config_state().phase == ConfigPhase::Armed);
    reboot.poll(60000, true);
    assert(reboot.config_state().phase == ConfigPhase::Testing);
    online(w2, s2);
    assert(reboot.accept(command(ConfigAction::Confirm, 4, 0, 4), 60001));
    assert(reboot.config_state().active_revision == 4);
}
void failures() {
    Store store;
    Wifi w;
    Server s;
    ConfigManager m(id, store, w, s);
    assert(m.begin(bundle(), true));
    assert(m.apply_active(0));
    const auto count = w.changes;
    store.fail = true;
    assert(!m.accept(command(ConfigAction::Apply, 1), 1));
    assert(w.changes == count && m.config_state().highwater == 0 && !m.configuration_healthy());
    store.fail = false;
    ConfigManager reboot(id, store, w, s);
    assert(reboot.begin({}, true));
    assert(reboot.apply_active(0));
    reboot.set_busy(true);
    assert(!reboot.accept(command(ConfigAction::Apply, 1), 1));
    assert(!reboot.local_wifi(bundle().wifi, 2));
    reboot.set_busy(false);
    assert(reboot.accept(command(ConfigAction::Apply, 1), 3));
    store.fail = true;
    reboot.poll(120003, true);
    assert(!reboot.configuration_healthy() && w.applied.ssid == bundle().wifi.ssid);
    store.fail = false;
    ConfigManager again(id, store, w, s);
    assert(again.begin({}, true));
    assert(again.config_state().error == ConfigError::Reboot);
    assert(again.config_state().highwater == 1);
    store.corrupt = true;
    ConfigManager broken(id, store, w, s);
    assert(!broken.begin(bundle(), true));
    assert(!broken.can_poll(0));
    assert(broken.recover_configuration(1));
    assert(!broken.config_state().management.enrolled);
    store.corrupt = false;
}
void failed_confirmation_rolls_back() {
    Store store;
    Wifi w;
    Server s;
    ConfigManager m(id, store, w, s);
    assert(m.begin(bundle(), true));
    assert(m.apply_active(0));
    assert(m.accept(command(ConfigAction::Apply, 1), 1));
    online(w, s);
    store.fail = true;
    assert(!m.accept(command(ConfigAction::Confirm, 1, 0, 1), 2));
    assert(!m.configuration_healthy());
    m.poll(120001, false);
    assert(w.applied.ssid == bundle().wifi.ssid);
    const auto changes = w.changes;
    m.poll(120002, false);
    assert(w.changes == changes); // No tight retry loop on failed flash.
}
void local() {
    Store store;
    Wifi w;
    Server s;
    ConfigManager m(id, store, w, s);
    assert(m.begin(bundle(), true));
    assert(m.apply_active(0));
    ManagementEnrollment e;
    e.set_endpoint("http://example.org:8000/management/v1/90:da:72:9b:cd:4c");
    e.key.fill(1);
    e.enrolled = e.enabled = true;
    assert(m.enroll_management(e, 0));
    assert(m.config_state().management.enabled);
    assert(m.accept(command(ConfigAction::Stage, 1), 30001));
    assert(m.local_server(bundle().server, 30002));
    assert(m.config_state().phase == ConfigPhase::Idle && m.config_state().highwater == 2);
    assert(m.enable_management(false, 30003));
    assert(!m.config_state().management.enabled);
    assert(m.forget_management(30004));
    assert(!m.config_state().management.enrolled && m.config_state().highwater == 2);
}
void protocol() {
    ManagementResponse p{};
    std::memcpy(p.data(), "GP01", 4);
    ManagementChallenge challenge{};
    challenge.fill(0x42);
    std::memcpy(p.data() + 48, challenge.data(), 16);
    ConfigCommand c;
    assert(decode_management_response(p, challenge, c));
    assert(c.action == ConfigAction::None);
    p[500] = 1;
    assert(!decode_management_response(p, challenge, c));
    p[500] = 0;
    challenge[0] ^= 1;
    assert(!decode_management_response(p, challenge, c));
}
void usb() {
    struct Legacy : WifiConfigStore, ServerConfigStore {
        ConfigLoadResult load_wifi(WifiCredentials &) override { return ConfigLoadResult::Defaults; }
        ConfigLoadResult load_server(ServerConfig &) override { return ConfigLoadResult::Defaults; }
        bool save_wifi(const WifiCredentials &) override {
            assert(false);
            return false;
        }
        bool save_server(const ServerConfig &) override {
            assert(false);
            return false;
        }
        bool forget_wifi() override {
            assert(false);
            return false;
        }
        bool forget_server() override {
            assert(false);
            return false;
        }
    } legacy;
    Store store;
    Wifi w;
    Server s;
    ConfigManager m(id, store, w, s);
    assert(m.begin(bundle(), true));
    assert(m.apply_active(0));
    WifiProvisioner usb(legacy, w, &legacy, &s, nullptr, nullptr, &m);
    int64_t now = 100;
    const auto send = [&](const std::string &line) {
        for (char c : line)
            usb.feed(c, now);
        return usb.feed('\n', now);
    };
    assert(send("manage status") == ProvisionReply::ManageStatus);
    assert(send("manage url http://example.org:8000/management/v1/90:da:72:9b:cd:4c") ==
           ProvisionReply::ManageField);
    assert(send("manage key " + std::string(64, '1')) == ProvisionReply::ManageField);
    assert(send("manage save") == ProvisionReply::ConfigSaved);
    assert(m.config_state().management.enrolled);
    m.set_busy(true);
    assert(send("wifi ssid test") == ProvisionReply::ConfigBusy);
    m.set_busy(false);
    now = 30101;
    assert(m.accept(command(ConfigAction::Apply, 1), now));
    const auto report_before_invalid = m.config_state().report_sequence;
    for (const char *invalid : {"wifi password short", "wifi unknown", "server address bad/host",
                                "server port 0", "manage url invalid"}) {
        assert(send(invalid) == ProvisionReply::Invalid);
        assert(m.config_state().phase == ConfigPhase::Testing);
        assert(m.config_state().report_sequence == report_before_invalid);
    }
    assert(send("wifi ssid local-network") == ProvisionReply::Ssid);
    assert(m.config_state().error == ConfigError::LocalOverride);
    assert(send("wifi password local-password") == ProvisionReply::Password);
    assert(send("wifi save") == ProvisionReply::Saved);
    assert(m.config_state().active.wifi.ssid == w.applied.ssid && m.config_state().active_revision == 2);
    assert(send("server address example.org") == ProvisionReply::ServerAddress);
    assert(send("server port 54321") == ProvisionReply::ServerPort);
    assert(send("server save") == ProvisionReply::ServerSaved);
    assert(send("manage url http://example.org") == ProvisionReply::ManageField);
    assert(send("manage key invalid") == ProvisionReply::Invalid);
    assert(send("manage save") == ProvisionReply::Invalid);
    assert(usb.tick(now + 300001) == ProvisionReply::Timeout);
    now += 300002;
    assert(send("manage save") == ProvisionReply::Invalid);
    assert(send("manage forget") == ProvisionReply::ConfigSaved);
    assert(!m.config_state().management.enrolled);
    for (bool long_line : {false, true}) {
        assert(send("wifi ssid unfinished") == ProvisionReply::Ssid);
        assert(send("wifi password unfinished-password") == ProvisionReply::Password);
        const auto before = m.config_state().report_sequence;
        if (long_line) {
            bool rejected = false;
            for (std::size_t i = 0; i < settings::kProvisionLineCapacity + 10; ++i)
                rejected = usb.feed('x', now) == ProvisionReply::TooLong || rejected;
            assert(rejected);
        } else
            assert(usb.feed('\x01', now) == ProvisionReply::Invalid);
        usb.feed('\n', now);
        assert(send("wifi save") == ProvisionReply::Invalid);
        assert(m.config_state().report_sequence == before);
    }
    assert(send("config recover") == ProvisionReply::ConfigSaved);
    assert(m.config_state().highwater == 0 && !m.config_state().management.enrolled);
}
} // namespace
int main(int argc, char **) {
    if (argc > 1) {
        ManagementResponse p{};
        std::cin.read(reinterpret_cast<char *>(p.data()), p.size());
        ManagementChallenge challenge{};
        challenge.fill(0x42);
        ConfigCommand c;
        if (std::cin.gcount() != 512 || !decode_management_response(p, challenge, c))
            return 2;
        std::cout << int(c.action) << " " << c.revision << " " << int(c.patch.present) << " "
                  << std::strlen(c.patch.wifi.password.data()) << " " << c.patch.server.host.data() << " "
                  << c.patch.server.port;
        if (c.action == ConfigAction::Sampling || c.action == ConfigAction::OtaCheck)
            std::cout << " " << c.interval_ms;
        return 0;
    }
    runtime_commands();
    codec();
    transitions();
    staged_and_armed();
    failures();
    failed_confirmation_rolls_back();
    local();
    protocol();
    usb();
    std::cout << "Configuration tests passed\n";
}
