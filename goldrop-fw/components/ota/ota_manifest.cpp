#include "goldrop/ota_manifest.hpp"
#include "goldrop/binary_codec.hpp"
#include "cJSON.h"
#include <cmath>
#include <cstring>
#include <memory>
namespace goldrop {
const char *update_decision_name(UpdateDecision decision) {
    switch (decision) {
    case UpdateDecision::Install:
        return "install";
    case UpdateDecision::SameOrOlder:
        return "same_or_older";
    case UpdateDecision::WrongHardware:
        return "wrong_hardware";
    case UpdateDecision::WrongChannel:
        return "wrong_channel";
    case UpdateDecision::WrongDevice:
        return "wrong_device";
    case UpdateDecision::TooLarge:
        return "too_large";
    case UpdateDecision::BadUrl:
        return "bad_http_origin";
    case UpdateDecision::Blocked:
        return "blocked_release";
    }
    return "unknown";
}
bool Version::parse(std::string_view text) {
    Version parsed;
    for (unsigned i = 0; i < 3; ++i) {
        const auto dot = text.find('.');
        const auto part = text.substr(0, dot);
        if (part.empty() || part.size() > 9 || (part.size() > 1 && part[0] == '0'))
            return false;
        for (char c : part) {
            if (c < '0' || c > '9')
                return false;
            parsed.parts[i] = parsed.parts[i] * 10 + uint32_t(c - '0');
        }
        if (i < 2) {
            if (dot == text.npos)
                return false;
            text.remove_prefix(dot + 1);
        } else if (dot != text.npos)
            return false;
    }
    *this = parsed;
    return true;
}
int Version::compare(const Version &o) const {
    for (unsigned i = 0; i < 3; ++i)
        if (parts[i] != o.parts[i])
            return parts[i] > o.parts[i] ? 1 : -1;
    return 0;
}
namespace {
template <std::size_t N> bool string_field(cJSON *root, const char *name, char (&value)[N]) {
    const auto *item = cJSON_GetObjectItemCaseSensitive(root, name);
    if (!cJSON_IsString(item) || !item->valuestring)
        return false;
    const auto size = std::strlen(item->valuestring);
    if (!size || size >= N)
        return false;
    for (std::size_t i = 0; i < size; ++i)
        if (item->valuestring[i] < 0x20 || item->valuestring[i] > 0x7e)
            return false;
    std::memcpy(value, item->valuestring, size + 1);
    return true;
}
int hex(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
struct JsonDeleter {
    void operator()(cJSON *value) const { cJSON_Delete(value); }
};
using Json = std::unique_ptr<cJSON, JsonDeleter>;
Json parse_document(std::string_view json) {
    if (json.empty() || json.size() > kManifestLimit || json.find('\0') != json.npos ||
        json.find("\\u0000") != json.npos)
        return {};
    // Bound nesting before invoking cJSON, independent of its library build options.
    unsigned depth = 0;
    bool quoted = false, escaped = false;
    for (char c : json) {
        if (quoted) {
            if (escaped)
                escaped = false;
            else if (c == '\\')
                escaped = true;
            else if (c == '"')
                quoted = false;
        } else if (c == '"')
            quoted = true;
        else if (c == '{' || c == '[') {
            if (++depth > 4)
                return {};
        } else if (c == '}' || c == ']') {
            if (!depth)
                return {};
            --depth;
        }
    }
    const char *end = nullptr;
    Json root(cJSON_ParseWithLengthOpts(json.data(), json.size(), &end, false));
    if (!root)
        return {};
    if (!cJSON_IsObject(root.get()))
        return {};
    while (end < json.data() + json.size() && (*end == ' ' || *end == '\n' || *end == '\r' || *end == '\t'))
        ++end;
    if (end != json.data() + json.size())
        return {};
    unsigned fields = 0;
    for (auto *a = root->child; a; a = a->next) {
        if (++fields > 16 || !a->string)
            return {};
        for (auto *b = a->next; b; b = b->next)
            if (b->string && std::strcmp(a->string, b->string) == 0)
                return {};
    }
    return root;
}
} // namespace
bool parse_manifest(std::string_view json, OtaManifest &output) {
    output = {};
    auto document = parse_document(json);
    if (!document)
        return false;
    auto *root = document.get();
    OtaManifest m;
    const auto *schema = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    if (!cJSON_IsNumber(schema) || schema->valuedouble != 1)
        return false;
    if (!string_field(root, "firmware_version", m.version) ||
        !string_field(root, "hardware_revision", m.hardware) || !string_field(root, "channel", m.channel) ||
        !string_field(root, "binary_url", m.url))
        return false;
    Version version;
    HttpUrl url;
    OtaConfig config;
    if (!version.parse(m.version) || !url.parse(m.url) || !config.set_channel(m.channel))
        return false;
    const auto *size = cJSON_GetObjectItemCaseSensitive(root, "size_bytes");
    if (!cJSON_IsNumber(size) || !std::isfinite(size->valuedouble) ||
        size->valuedouble < kFirmwarePrefixSize || size->valuedouble > 1572864 ||
        std::floor(size->valuedouble) != size->valuedouble)
        return false;
    m.size = static_cast<uint32_t>(size->valuedouble);
    char sha[65];
    if (!string_field(root, "sha256", sha) || std::strlen(sha) != 64)
        return false;
    for (unsigned i = 0; i < 32; ++i) {
        const int a = hex(sha[2 * i]), b = hex(sha[2 * i + 1]);
        if (a < 0 || b < 0)
            return false;
        m.sha256[i] = uint8_t(a * 16 + b);
    }
    const auto *mandatory = cJSON_GetObjectItemCaseSensitive(root, "mandatory");
    if (mandatory && !cJSON_IsBool(mandatory))
        return false;
    m.mandatory = cJSON_IsTrue(mandatory);
    const auto *targets = cJSON_GetObjectItemCaseSensitive(root, "target_device_ids");
    if (targets) {
        if (!cJSON_IsArray(targets) || cJSON_GetArraySize(targets) > 32)
            return false;
        m.targets_present = true;
        for (const auto *t = targets->child; t; t = t->next) {
            if (!cJSON_IsString(t) || !t->valuestring || std::strlen(t->valuestring) != 17)
                return false;
            DeviceId id;
            for (unsigned i = 0; i < 6; ++i) {
                const int a = hex(t->valuestring[3 * i]), b = hex(t->valuestring[3 * i + 1]);
                if (a < 0 || b < 0 || (i < 5 && t->valuestring[3 * i + 2] != ':'))
                    return false;
                id.mac[i] = uint8_t(a * 16 + b);
            }
            if (!id.valid())
                return false;
            m.targets[m.target_count++] = id;
        }
    }
    output = m;
    return true;
}
bool read_update_manifest(std::string_view json, bool required, ManifestSignatureVerifier &verifier,
                          OtaManifest &output, const char *&error) {
    output = {};
    error = "manifest_invalid";
    auto document = parse_document(json);
    if (!document)
        return false;
    auto *root = document.get();
    const auto *payload = cJSON_GetObjectItemCaseSensitive(root, "signed_manifest");
    const auto *schema = cJSON_GetObjectItemCaseSensitive(root, "signature_schema");
    const auto *signature = cJSON_GetObjectItemCaseSensitive(root, "signature_hex");
    if (!payload && !schema && !signature) {
        if (required) {
            error = "unsigned_http_manifest";
            return false;
        }
        return parse_manifest(json, output);
    }
    error = "signature_envelope";
    unsigned fields = 0;
    for (auto *field = root->child; field; field = field->next)
        ++fields;
    char algorithm[32], encoded[145];
    if (fields != 4 || !cJSON_IsNumber(schema) || schema->valuedouble != 1 ||
        !string_field(root, "algorithm", algorithm) || std::strcmp(algorithm, "ecdsa-p256-sha256") ||
        !cJSON_IsString(payload) || !payload->valuestring || !string_field(root, "signature_hex", encoded))
        return false;
    const auto count = std::strlen(encoded);
    if (count < 16 || count % 2)
        return false;
    std::array<uint8_t, 72> bytes{};
    for (unsigned i = 0; i < count / 2; ++i) {
        const auto a = hex(encoded[2 * i]), b = hex(encoded[2 * i + 1]);
        if (a < 0 || b < 0)
            return false;
        bytes[i] = static_cast<uint8_t>(a * 16 + b);
    }
    if (!verifier.available()) {
        error = "signature_key_missing";
        return false;
    }
    if (!verifier.verify(payload->valuestring, bytes.data(), count / 2)) {
        error = "signature_invalid";
        return false;
    }
    error = "manifest_invalid";
    return parse_manifest(payload->valuestring, output);
}
UpdateDecision check_update(const OtaManifest &m, const OtaConfig &c, DeviceId id, std::string_view current,
                            std::string_view hardware, std::size_t slot,
                            const std::array<uint8_t, 32> *blocked) {
    Version a, b;
    if (!a.parse(m.version) || !b.parse(current) || a.compare(b) <= 0)
        return UpdateDecision::SameOrOlder;
    if (m.hardware != hardware)
        return UpdateDecision::WrongHardware;
    if (std::strcmp(m.channel, c.channel.data()))
        return UpdateDecision::WrongChannel;
    if (m.targets_present) {
        bool found = false;
        for (unsigned i = 0; i < m.target_count; ++i)
            if (m.targets[i] == id)
                found = true;
        if (!found || !id.valid())
            return UpdateDecision::WrongDevice;
    }
    if (m.size > slot)
        return UpdateDecision::TooLarge;
    if (!same_http_origin(c.manifest_url.data(), m.url))
        return UpdateDecision::BadUrl;
    if (blocked && *blocked == m.sha256)
        return UpdateDecision::Blocked;
    return UpdateDecision::Install;
}
bool validate_firmware_prefix(const uint8_t *data, std::size_t size, const OtaManifest &m) {
    if (!data || size < kFirmwarePrefixSize || data[0] != 0xe9 || data[12] != 5 || data[13] != 0)
        return false; // ESP32-C3 chip ID.
    if (binary::get32(data + 32) != 0xabcd5432 ||
        std::memcmp(data + 48, m.version, std::strlen(m.version) + 1))
        return false;
    if (std::memcmp(data + 80, "goldrop", 8))
        return false;
    const auto *custom = data + kFirmwareDescriptorOffset;
    return std::memcmp(custom, "GDROPFW1", 8) == 0 && binary::get32(custom + 8) == 1 &&
           std::memcmp(custom + 12, m.hardware, std::strlen(m.hardware) + 1) == 0 &&
           std::memcmp(custom + 44, m.version, std::strlen(m.version) + 1) == 0 &&
           binary::get32(custom + 76) == 0;
}
} // namespace goldrop
