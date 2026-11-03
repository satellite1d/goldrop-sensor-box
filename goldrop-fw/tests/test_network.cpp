#include "goldrop/wifi_config_codec.hpp"
#include "goldrop/wifi_provisioner.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {
using namespace goldrop;
void require(bool condition, const char* expression, int line) {
    if (!condition) {
        std::fprintf(stderr, "FAIL network test line %d: %s\n", line, expression);
        std::exit(1);
    }
}
#define REQUIRE(expression) require((expression), #expression, __LINE__)

WifiCredentials credentials() {
    WifiCredentials config;
    REQUIRE(config.set_ssid("Test network", 12));
    REQUIRE(config.set_password("example-pass", 12));
    return config;
}
class MemoryConfig final : public WifiConfigStore {
public:
    WifiCredentials saved;
    bool exists{}, writable{true};
    unsigned writes{};
    ConfigLoadResult load_wifi(WifiCredentials& result) override {
        result = saved;
        return exists ? ConfigLoadResult::Loaded : ConfigLoadResult::Defaults;
    }
    bool save_wifi(const WifiCredentials& config) override {
        if (!writable) return false;
        saved = config; exists = true; ++writes;
        return true;
    }
    bool forget_wifi() override {
        if (!writable) return false;
        saved.clear(); exists = false;
        return true;
    }
};
class FakeNetwork final : public NetworkControl {
public:
    WifiCredentials applied;
    unsigned configurations{}, disables{};
    bool configure(const WifiCredentials& config, int64_t) override {
        applied = config; ++configurations; return true;
    }
    void disable(int64_t) override { applied.clear(); ++disables; }
    NetworkStatus status() const override { return {}; }
};
ProvisionReply send(WifiProvisioner& protocol, const std::string& line, int64_t now = 0) {
    ProvisionReply result = ProvisionReply::None;
    for (const char byte : line) {
        const ProvisionReply received = protocol.feed(byte, now++);
        if (received != ProvisionReply::None) result = received;
    }
    return result;
}

void test_credentials_and_persistent_format() {
    WifiCredentials config = credentials();
    REQUIRE(config.valid());
    WifiConfigBlob blob;
    REQUIRE(encode_wifi_config(config, blob));
    REQUIRE(blob[4] == 1 && blob[5] == 12 && blob[6] == 12 && blob[7] == 0);
    WifiCredentials restored;
    REQUIRE(decode_wifi_config(blob.data(), blob.size(), restored));
    REQUIRE(restored.ssid == config.ssid && restored.password == config.password);
    REQUIRE(!decode_wifi_config(blob.data(), blob.size() - 1, restored) && !restored.valid());
    for (const std::size_t offset : {0, 4, 5, 6, 7}) {
        auto corrupt = blob;
        corrupt[offset] = 255;
        REQUIRE(!decode_wifi_config(corrupt.data(), corrupt.size(), restored));
    }
    auto corrupt = blob;
    corrupt[40] = 0;  // Embedded NUL in declared password.
    REQUIRE(!decode_wifi_config(corrupt.data(), corrupt.size(), restored));
    const std::string longest_ssid(32, 's'), longest_password(63, 'p');
    REQUIRE(config.set_ssid(longest_ssid.data(), longest_ssid.size()));
    REQUIRE(config.set_password(longest_password.data(), longest_password.size()));
    REQUIRE(config.valid() && encode_wifi_config(config, blob));
    REQUIRE(decode_wifi_config(blob.data(), blob.size(), restored));
    REQUIRE(restored.ssid == config.ssid && restored.password == config.password);
    REQUIRE(!config.set_ssid("", 0));
    REQUIRE(!config.set_ssid(longest_ssid.data(), 33));
    REQUIRE(!config.set_password("short", 5));
    REQUIRE(!config.set_password(longest_password.data(), 64));
    REQUIRE(!config.set_password("example\npass", 12));
    REQUIRE(config.set_ssid("Siec-test", 9));
    REQUIRE(config.set_password("another-pass", 12));
    REQUIRE(encode_wifi_config(config, blob));
    for (std::size_t i = 8 + 9; i < 40; ++i) REQUIRE(blob[i] == 0);
    for (std::size_t i = 40 + 12; i < blob.size(); ++i) REQUIRE(blob[i] == 0);
    config.clear();
    REQUIRE(!config.valid() && config.ssid[0] == 0 && config.password[0] == 0);
    config.ssid.fill('x'); config.password.fill('x');
    REQUIRE(!config.valid());  // Unterminated buffers never reach strlen().
}

void test_wifi_retry_policy() {
    WifiRetryPolicy policy;
    REQUIRE(policy.poll(1'000'000) == WifiAction::None);
    REQUIRE(!policy.got_ip(0));  // Stale IP cannot make an unconfigured station online.
    policy.starting(0); policy.started(1);
    REQUIRE(policy.poll(1) == WifiAction::Retry);
    REQUIRE(policy.poll(20'000) == WifiAction::None);
    REQUIRE(policy.poll(20'001) == WifiAction::Disconnect);
    REQUIRE(policy.retry_delay_ms(20'001) == 1'000);
    policy.failed(20'010);  // Duplicate disconnect does not postpone the retry.
    REQUIRE(policy.retry_delay_ms(20'010) == 991);
    REQUIRE(policy.poll(21'001) == WifiAction::Retry);
    policy.failed(21'002);
    REQUIRE(policy.retry_delay_ms(21'002) == 2'000);
    REQUIRE(policy.poll(23'002) == WifiAction::Retry);
    policy.associated(23'003);
    REQUIRE(policy.state() == WifiState::WaitingIp);
    REQUIRE(policy.poll(38'002) == WifiAction::None);
    REQUIRE(policy.poll(38'003) == WifiAction::Disconnect);  // DHCP has a deadline.
    int64_t now = 40'000;
    for (unsigned i = 0; i < 30; ++i) {
        const uint32_t delay = policy.retry_delay_ms(now);
        REQUIRE(delay <= 60'000);
        now += delay + 60'000;
        REQUIRE(policy.poll(now) == WifiAction::Retry);
        policy.failed(now, 987'654);
        REQUIRE(policy.retry_delay_ms(now) >= 1'000 && policy.retry_delay_ms(now) <= 60'000);
    }
    now += 60'000;
    REQUIRE(policy.poll(now) == WifiAction::Retry);
    policy.associated(now);
    REQUIRE(policy.got_ip(now));
    policy.failed(now + 5'000);
    REQUIRE(policy.retry_delay_ms(now + 5'000) == 60'000);  // Brief connection does not reset backoff.
    now += 65'000;
    REQUIRE(policy.poll(now) == WifiAction::Retry);
    policy.associated(now); REQUIRE(policy.got_ip(now));
    REQUIRE(policy.poll(now + 30'000) == WifiAction::None);
    policy.failed(now + 30'001);
    REQUIRE(policy.retry_delay_ms(now + 30'001) == 1'000);
    policy.stopping(now);
    REQUIRE(!policy.got_ip(now));
    policy.failed(now); REQUIRE(policy.state() == WifiState::Stopping);
    REQUIRE(policy.poll(now + 5'000) == WifiAction::Stop);
    policy.reset();
    REQUIRE(policy.poll(now + 100'000) == WifiAction::None);
}

void test_usb_configuration_commit_and_framing() {
    MemoryConfig store;
    FakeNetwork network;
    WifiProvisioner protocol(store, network);
    REQUIRE(send(protocol, "wifi save\n") == ProvisionReply::Invalid);
    REQUIRE(send(protocol, "wifi ssid Test network\r\n") == ProvisionReply::Ssid);
    REQUIRE(send(protocol, "wifi save\n") == ProvisionReply::Invalid);
    REQUIRE(send(protocol, "wifi password example-pass\n") == ProvisionReply::Password);
    REQUIRE(!store.exists && network.configurations == 0);  // Staging has no side effects.
    REQUIRE(send(protocol, "wifi password short\n") == ProvisionReply::Invalid);
    REQUIRE(send(protocol, "wifi save\n") == ProvisionReply::Invalid && !store.exists);
    REQUIRE(send(protocol, "wifi password example-pass\n") == ProvisionReply::Password);
    REQUIRE(send(protocol, "wifi save\n") == ProvisionReply::Saved);
    REQUIRE(store.exists && store.writes == 1 && network.configurations == 1);
    REQUIRE(std::strcmp(network.applied.ssid.data(), "Test network") == 0);
    REQUIRE(std::strcmp(network.applied.password.data(), "example-pass") == 0);
    REQUIRE(send(protocol, "wifi save\n") == ProvisionReply::Invalid);  // Staged secrets were cleared.
    REQUIRE(send(protocol, "wifi status\n") == ProvisionReply::Status);
    REQUIRE(send(protocol, "wifi help\n") == ProvisionReply::Help);

    REQUIRE(send(protocol, "wifi ssid Changed\n") == ProvisionReply::Ssid);
    REQUIRE(send(protocol, "wifi password new-password\n") == ProvisionReply::Password);
    store.writable = false;
    REQUIRE(send(protocol, "wifi save\n") == ProvisionReply::StorageError);
    REQUIRE(network.configurations == 1 && std::strcmp(store.saved.ssid.data(), "Test network") == 0);
    REQUIRE(send(protocol, "wifi forget\n") == ProvisionReply::StorageError && network.disables == 0);
    store.writable = true;
    REQUIRE(send(protocol, "wifi save\n") == ProvisionReply::Saved && network.configurations == 2);

    REQUIRE(send(protocol, std::string(settings::kProvisionLineCapacity + 1, 'x') + "wifi forget\n") == ProvisionReply::TooLong);
    REQUIRE(store.exists && network.disables == 0);  // Oversized frame tail is never executed.
    REQUIRE(send(protocol, "wifi status\n") == ProvisionReply::Status);
    REQUIRE(send(protocol, "wifi password partial", 1'000) == ProvisionReply::None);
    REQUIRE(protocol.tick(7'000) == ProvisionReply::Timeout);
    REQUIRE(send(protocol, "wifi forget\n", 7'001) == ProvisionReply::None);
    REQUIRE(store.exists && network.disables == 0);
    REQUIRE(send(protocol, "wifi ssid Uncommitted\n", 10'000) == ProvisionReply::Ssid);
    protocol.cancel();  // USB detaches.
    REQUIRE(send(protocol, "wifi password example-pass\n", 10'100) == ProvisionReply::Password);
    REQUIRE(send(protocol, "wifi save\n", 10'200) == ProvisionReply::Invalid);
    REQUIRE(protocol.tick(400'000) == ProvisionReply::Timeout);
    REQUIRE(send(protocol, "wifi forget\n", 400'001) == ProvisionReply::Forgotten);
    REQUIRE(!store.exists && network.disables == 1);
}
}  // namespace

void test_network_and_configuration() {
    test_credentials_and_persistent_format();
    test_wifi_retry_policy();
    test_usb_configuration_commit_and_framing();
}
