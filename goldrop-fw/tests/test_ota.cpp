#include "goldrop/ota_manifest.hpp"
#include "goldrop/ota_image_stream.hpp"
#include "goldrop/ota_config_store.hpp"
#include "goldrop/wifi_provisioner.hpp"
#include "goldrop/binary_codec.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cstring>
#include <vector>
namespace {
using namespace goldrop;
void require(bool v, int line) {
    if (!v) {
        std::fprintf(stderr, "FAIL OTA test line %d\n", line);
        std::exit(1);
    }
}
#define REQUIRE(...) require((__VA_ARGS__), __LINE__)
std::string manifest(std::string version = "0.2.1") {
    return "{\"schema_version\":1,\"firmware_version\":\"" + version +
           "\",\"hardware_revision\":\"goldrop-c3-v1.0\",\"channel\":\"stable\",\"binary_url\":\"https://"
           "updates.example/fw.bin\",\"size_bytes\":4096,\"sha256\":\"" +
           std::string(64, 'a') + "\",\"mandatory\":true}";
}
class Config final : public WifiConfigStore, public OtaConfigStore {
  public:
    OtaConfig saved;
    bool writable{true}, exists{};
    unsigned writes{};
    ConfigLoadResult load_wifi(WifiCredentials &) override { return ConfigLoadResult::Defaults; }
    bool save_wifi(const WifiCredentials &) override { return true; }
    bool forget_wifi() override { return true; }
    ConfigLoadResult load_ota(OtaConfig &c) override {
        c = saved;
        return exists ? ConfigLoadResult::Loaded : ConfigLoadResult::Defaults;
    }
    bool save_ota(const OtaConfig &c) override {
        if (!writable)
            return false;
        saved = c;
        exists = true;
        ++writes;
        return true;
    }
    bool forget_ota() override {
        if (!writable)
            return false;
        exists = false;
        return true;
    }
};
class Network final : public NetworkControl {
    bool configure(const WifiCredentials &, int64_t) override { return true; }
    void disable(int64_t) override {}
    NetworkStatus status() const override { return {}; }
};
class Service final : public OtaService {
  public:
    bool http{};
    bool signed_http_supported() const override { return http; }
    unsigned applied{}, checks{}, retries{}, disables{};
    OtaState state() const override { return OtaState::Idle; }
    bool request_check() override {
        ++checks;
        return true;
    }
    bool configure_ota(const OtaConfig &, int64_t) override {
        ++applied;
        return true;
    }
    void disable_ota() override { ++disables; }
    bool clear_block() override {
        ++retries;
        return true;
    }
    unsigned progress() const override { return 0; }
    const char *last_error() const override { return "none"; }
};
class Verifier final : public ManifestSignatureVerifier {
  public:
    bool key{true};
    unsigned calls{};
    std::string expected;
    bool available() const override { return key; }
    bool verify(std::string_view payload, const uint8_t *signature, std::size_t size) override {
        ++calls;
        return payload == expected && size == 8 && signature[0] == 1 && signature[7] == 8;
    }
};
std::string envelope(std::string_view payload, std::string_view signature = "0102030405060708") {
    std::string escaped;
    for (char c : payload) {
        if (c == '\\' || c == '"')
            escaped += '\\';
        escaped += c;
    }
    return "{\"signature_schema\":1,\"algorithm\":\"ecdsa-p256-sha256\",\"signed_manifest\":\"" + escaped +
           "\",\"signature_hex\":\"" + std::string(signature) + "\"}";
}
class Writer final : public OtaImageWriter {
  public:
    std::vector<uint8_t> bytes;
    bool writable{true};
    bool write(const uint8_t *data, std::size_t size) override {
        if (!writable)
            return false;
        bytes.insert(bytes.end(), data, data + size);
        return true;
    }
};
ProvisionReply send(WifiProvisioner &p, std::string text) {
    ProvisionReply result{};
    for (char c : text) {
        auto r = p.feed(c, 100);
        if (r != ProvisionReply::None)
            result = r;
    }
    return result;
}
} // namespace
void test_ota_features() {
    using namespace goldrop;
    Version a, b;
    REQUIRE(a.parse("1.10.2") && b.parse("1.2.99") && a.compare(b) > 0);
    for (const auto *s : {"1.2", "01.2.3", "1.2.3.4", "1.-2.3", "1.2.3-beta", "1000000000.2.3"})
        REQUIRE(!a.parse(s));
    OtaConfig c;
    REQUIRE(c.set_url("https://updates.example/manifest.json"));
    REQUIRE(c.valid());
    REQUIRE(c.set_url("http://updates.example/m.json"));
    HttpUrl http_url;
    REQUIRE(http_url.parse(c.manifest_url.data()) && !http_url.tls && http_url.origin.port == 80);
    REQUIRE(same_http_origin(c.manifest_url.data(), "http://UPDATES.example:80/fw.bin"));
    REQUIRE(!same_http_origin(c.manifest_url.data(), "https://updates.example/fw.bin"));
    REQUIRE(c.set_url("https://updates.example/manifest.json"));
    REQUIRE(!c.set_url("https://user:pass@updates.example/m"));
    REQUIRE(same_https_origin(c.manifest_url.data(), "https://UPDATES.example:443/x.bin"));
    REQUIRE(!same_https_origin(c.manifest_url.data(), "https://other.example/x.bin"));
    REQUIRE(!same_https_origin(c.manifest_url.data(), "https://updates.example:444/x.bin"));
    OtaConfigBlob blob;
    REQUIRE(encode_ota_config(c, blob));
    OtaConfig copy;
    REQUIRE(decode_ota_config(blob.data(), blob.size(), copy) && copy.manifest_url == c.manifest_url);
    auto bad = blob;
    bad[4] = 3;
    REQUIRE(!decode_ota_config(bad.data(), bad.size(), copy));
    auto legacy = blob;
    legacy[4] = 1;
    REQUIRE(decode_ota_config(legacy.data(), legacy.size(), copy));
    bad = blob;
    std::memset(bad.data() + 12, 'x', 384);
    REQUIRE(!decode_ota_config(bad.data(), bad.size(), copy));
    OtaManifest m;
    REQUIRE(parse_manifest(manifest(), m));
    DeviceId id{{2, 0, 0, 0, 0, 1}};
    REQUIRE(check_update(m, c, id, "0.2.0", kHardwareRevision, 1572864, nullptr) == UpdateDecision::Install);
    REQUIRE(check_update(m, c, id, "0.2.1", kHardwareRevision, 1572864, nullptr) ==
            UpdateDecision::SameOrOlder);
    REQUIRE(check_update(m, c, id, "0.3.0", kHardwareRevision, 1572864, nullptr) ==
            UpdateDecision::SameOrOlder);
    REQUIRE(check_update(m, c, id, "0.2.0", "other-board", 1572864, nullptr) ==
            UpdateDecision::WrongHardware);
    REQUIRE(check_update(m, c, id, "0.2.0", kHardwareRevision, 100, nullptr) == UpdateDecision::TooLarge);
    REQUIRE(check_update(m, c, id, "0.2.0", kHardwareRevision, 1572864, &m.sha256) ==
            UpdateDecision::Blocked);
    const auto good = manifest();
    Verifier verifier;
    verifier.expected = good;
    const char *error{};
    REQUIRE(read_update_manifest(good, false, verifier, m, error) && verifier.calls == 0);
    REQUIRE(!read_update_manifest(good, true, verifier, m, error));
    REQUIRE(std::strcmp(error, "unsigned_http_manifest") == 0);
    REQUIRE(read_update_manifest(envelope(good), true, verifier, m, error) && verifier.calls == 1);
    REQUIRE(!read_update_manifest(envelope(good, "0102030405060709"), true, verifier, m, error));
    REQUIRE(std::strcmp(error, "signature_invalid") == 0);
    REQUIRE(!read_update_manifest(envelope(manifest("0.2.2")), true, verifier, m, error));
    verifier.key = false;
    REQUIRE(!read_update_manifest(envelope(good), true, verifier, m, error));
    REQUIRE(std::strcmp(error, "signature_key_missing") == 0);
    verifier.key = true;
    for (const auto *signature : {"", "010", "0123456789abcdefg0"})
        REQUIRE(!read_update_manifest(envelope(good, signature), true, verifier, m, error));
    REQUIRE(!read_update_manifest(envelope(good, std::string(146, '0')), true, verifier, m, error));
    auto duplicate_signature = envelope(good);
    duplicate_signature.insert(1, "\"signature_schema\":1,");
    REQUIRE(!read_update_manifest(duplicate_signature, true, verifier, m, error));
    verifier.expected = "{}";
    REQUIRE(!read_update_manifest(envelope("{}"), true, verifier, m, error));
    REQUIRE(!parse_manifest(good + "garbage", m));
    REQUIRE(!parse_manifest("{\"schema_version\":1," + good.substr(1), m));
    REQUIRE(!parse_manifest(std::string(9000, ' '), m));
    auto maximum = good;
    maximum.resize(kManifestLimit, ' ');
    REQUIRE(parse_manifest(maximum, m));
    maximum.push_back(' ');
    REQUIRE(!parse_manifest(maximum, m));
    REQUIRE(!parse_manifest("[[[[[{}]]]]]", m));
    for (const auto &change : std::vector<std::pair<std::string, std::string>>{
             {"\"schema_version\":1", "\"schema_version\":2"},
             {"\"firmware_version\":\"0.2.1\"", "\"firmware_version\":\"0.2\""},
             {"\"hardware_revision\":\"goldrop-c3-v1.0\"", "\"hardware_revision\":\"\""},
             {"\"channel\":\"stable\"", "\"channel\":\"invalid channel\""},
             {"https://updates.example/fw.bin", "ftp://updates.example/fw.bin"},
             {"\"size_bytes\":4096", "\"size_bytes\":367"},
             {"\"size_bytes\":4096", "\"size_bytes\":1572865"},
             {"\"size_bytes\":4096", "\"size_bytes\":4096.5"},
             {"\"size_bytes\":4096", "\"size_bytes\":\"4096\""},
             {std::string(64, 'a'), std::string(63, 'a')},
             {std::string(64, 'a'), std::string(64, 'g')},
             {"\"mandatory\":true", "\"mandatory\":\"true\""}}) {
        auto invalid = good;
        const auto position = invalid.find(change.first);
        REQUIRE(position != invalid.npos);
        invalid.replace(position, change.first.size(), change.second);
        REQUIRE(!parse_manifest(invalid, m));
    }
    auto target = good;
    target.insert(target.size() - 1, ",\"target_device_ids\":[\"02:00:00:00:00:01\"]");
    REQUIRE(parse_manifest(target, m));
    REQUIRE(check_update(m, c, id, "0.2.0", kHardwareRevision, 1572864, nullptr) == UpdateDecision::Install);
    ++id.mac[5];
    REQUIRE(check_update(m, c, id, "0.2.0", kHardwareRevision, 1572864, nullptr) ==
            UpdateDecision::WrongDevice);
    target.replace(target.find("02:00:00:00:00:01"), 17, "ff:ff:ff:ff:ff:ff");
    REQUIRE(!parse_manifest(target, m));
    REQUIRE(parse_manifest(good, m));
    auto other_origin = c;
    REQUIRE(other_origin.set_url("https://other.example/manifest.json"));
    REQUIRE(check_update(m, other_origin, id, "0.2.0", kHardwareRevision, 1572864, nullptr) ==
            UpdateDecision::BadUrl);
    REQUIRE(other_origin.set_url(c.manifest_url.data()) && other_origin.set_channel("beta"));
    REQUIRE(check_update(m, other_origin, id, "0.2.0", kHardwareRevision, 1572864, nullptr) ==
            UpdateDecision::WrongChannel);
    std::array<uint8_t, kFirmwarePrefixSize> prefix{};
    prefix[0] = 0xe9;
    prefix[12] = 5;
    binary::put32(prefix.data() + 32, 0xabcd5432);
    std::memcpy(prefix.data() + 48, m.version, std::strlen(m.version) + 1);
    std::memcpy(prefix.data() + 80, "goldrop", 8);
    auto *descriptor = prefix.data() + kFirmwareDescriptorOffset;
    std::memcpy(descriptor, "GDROPFW1", 8);
    binary::put32(descriptor + 8, 1);
    std::memcpy(descriptor + 12, m.hardware, std::strlen(m.hardware) + 1);
    std::memcpy(descriptor + 44, m.version, std::strlen(m.version) + 1);
    REQUIRE(validate_firmware_prefix(prefix.data(), prefix.size(), m));
    std::vector<uint8_t> image(m.size, 0x5a);
    std::copy(prefix.begin(), prefix.end(), image.begin());
    OtaImageStream incomplete(m);
    Writer untouched;
    REQUIRE(incomplete.accept(image.data(), 100, untouched));
    REQUIRE(!incomplete.complete() && untouched.bytes.empty());
    for (unsigned chunk : {1u, 7u, 367u, 368u, 1024u, 2048u}) {
        OtaImageStream stream(m);
        Writer writer;
        for (std::size_t offset = 0; offset < image.size();) {
            const auto amount = std::min<std::size_t>(chunk, image.size() - offset);
            REQUIRE(stream.accept(image.data() + offset, amount, writer));
            offset += amount;
        }
        REQUIRE(stream.complete() && writer.bytes == image);
        REQUIRE(!stream.accept(image.data(), 1, writer));
    }
    OtaImageStream broken_stream(m);
    Writer no_write;
    no_write.writable = false;
    REQUIRE(!broken_stream.accept(image.data(), image.size(), no_write) && !broken_stream.complete());
    descriptor[12] = 'X';
    REQUIRE(!validate_firmware_prefix(prefix.data(), prefix.size(), m));
    OtaImageStream wrong_hardware(m);
    Writer unchanged;
    REQUIRE(!wrong_hardware.accept(prefix.data(), prefix.size(), unchanged) && unchanged.bytes.empty());
    Config config;
    Network network;
    Service service;
    WifiProvisioner protocol(config, network, nullptr, nullptr, &config, &service);
    REQUIRE(send(protocol, "ota save\n") == ProvisionReply::Invalid);
    REQUIRE(send(protocol, "ota url https://updates.example/manifest.json\n") == ProvisionReply::OtaField);
    REQUIRE(send(protocol, "ota enable 1\n") == ProvisionReply::OtaField);
    config.writable = false;
    REQUIRE(send(protocol, "ota save\n") == ProvisionReply::StorageError && service.applied == 0);
    config.writable = true;
    REQUIRE(send(protocol, "ota save\n") == ProvisionReply::OtaSaved && config.saved.enabled &&
            service.applied == 1);
    REQUIRE(send(protocol, "ota check\n") == ProvisionReply::OtaCheck && service.checks == 1);
    REQUIRE(send(protocol, "ota retry\n") == ProvisionReply::OtaRetry && service.retries == 1);
    REQUIRE(send(protocol, "ota disable\n") == ProvisionReply::OtaDisabled && !config.saved.enabled);
    REQUIRE(send(protocol, "ota forget\n") == ProvisionReply::OtaForgotten && !config.exists &&
            service.disables == 1);
    REQUIRE(send(protocol, "ota url http://updates.example:8000/manifest.json\n") ==
            ProvisionReply::OtaField);
    REQUIRE(send(protocol, "ota save\n") == ProvisionReply::OtaRejected && !config.exists);
    service.http = true;
    REQUIRE(send(protocol, "ota save\n") == ProvisionReply::OtaSaved && config.exists);
    REQUIRE(encode_ota_config(config.saved, blob) && blob[4] == 2);
    REQUIRE(decode_ota_config(blob.data(), blob.size(), copy));
    blob[4] = 1;
    REQUIRE(!decode_ota_config(blob.data(), blob.size(), copy));
}
