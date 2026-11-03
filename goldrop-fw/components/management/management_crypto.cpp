#include "goldrop/management_crypto.hpp"
#include "psa/crypto.h"
#include "esp_random.h"
#if !defined(PSA_WANT_ALG_GCM) || !defined(PSA_WANT_ALG_HKDF) || !defined(PSA_WANT_ALG_SHA_256) ||           \
    !defined(PSA_WANT_KEY_TYPE_AES)
#error "Management requires PSA AES-GCM, HKDF and SHA-256"
#endif
namespace goldrop {
namespace {
struct Key {
    psa_key_id_t id{};
    ~Key() {
        if (id)
            psa_destroy_key(id);
    }
};
bool derive(const std::array<uint8_t, 32> &master, char direction, Key &key) {
    if (psa_crypto_init() != PSA_SUCCESS)
        return false;
    constexpr uint8_t salt[] = "goldrop-management-v1";
    const char *info = direction == 'C' ? "request" : "response";
    psa_key_derivation_operation_t op = PSA_KEY_DERIVATION_OPERATION_INIT;
    std::array<uint8_t, 32> raw{};
    bool ok = psa_key_derivation_setup(&op, PSA_ALG_HKDF(PSA_ALG_SHA_256)) == PSA_SUCCESS &&
              psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SALT, salt, sizeof(salt) - 1) ==
                  PSA_SUCCESS &&
              psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_SECRET, master.data(),
                                             master.size()) == PSA_SUCCESS &&
              psa_key_derivation_input_bytes(&op, PSA_KEY_DERIVATION_INPUT_INFO,
                                             reinterpret_cast<const uint8_t *>(info),
                                             std::strlen(info)) == PSA_SUCCESS &&
              psa_key_derivation_output_bytes(&op, raw.data(), raw.size()) == PSA_SUCCESS;
    psa_key_derivation_abort(&op);
    if (ok) {
        psa_key_attributes_t attrs = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&attrs, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&attrs, 256);
        psa_set_key_usage_flags(&attrs, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
        psa_set_key_algorithm(&attrs, PSA_ALG_GCM);
        ok = psa_import_key(&attrs, raw.data(), raw.size(), &key.id) == PSA_SUCCESS;
        psa_reset_key_attributes(&attrs);
    }
    clear_secret(raw.data(), raw.size());
    return ok;
}
auto aad(DeviceId id, char direction) {
    constexpr char prefix[] = "goldrop-config-v1";
    std::array<uint8_t, sizeof(prefix) + 7> out{};
    std::memcpy(out.data(), prefix, sizeof(prefix));
    std::memcpy(out.data() + sizeof(prefix), id.mac.data(), 6);
    out.back() = uint8_t(direction);
    return out;
}
} // namespace
bool management_encrypt(const std::array<uint8_t, 32> &master, DeviceId id, char direction, const uint8_t *in,
                        std::size_t n, uint8_t *out, std::size_t capacity, std::size_t &written) {
    written = 0;
    if (!id.valid() || (direction != 'C' && direction != 'S') || capacity < n + 32)
        return false;
    Key key;
    if (!derive(master, direction, key))
        return false;
    std::memcpy(out, "GRC1", 4);
    esp_fill_random(out + 4, 12);
    const auto context = aad(id, direction);
    if (psa_aead_encrypt(key.id, PSA_ALG_GCM, out + 4, 12, context.data(), context.size(), in, n, out + 16,
                         capacity - 16, &written) != PSA_SUCCESS) {
        clear_secret(out, capacity);
        written = 0;
        return false;
    }
    written += 16;
    return true;
}
bool management_decrypt(const std::array<uint8_t, 32> &master, DeviceId id, char direction, const uint8_t *in,
                        std::size_t n, uint8_t *out, std::size_t capacity, std::size_t &written) {
    written = 0;
    if (!id.valid() || (direction != 'C' && direction != 'S') || n < 32 || n - 32 > capacity ||
        std::memcmp(in, "GRC1", 4))
        return false;
    Key key;
    if (!derive(master, direction, key))
        return false;
    const auto context = aad(id, direction);
    if (psa_aead_decrypt(key.id, PSA_ALG_GCM, in + 4, 12, context.data(), context.size(), in + 16, n - 16,
                         out, capacity, &written) != PSA_SUCCESS) {
        clear_secret(out, capacity);
        written = 0;
        return false;
    }
    return true;
}
bool management_key_id(const std::array<uint8_t, 32> &key, char (&hex)[17]) {
    std::array<uint8_t, 32> hash{};
    std::size_t n = 0;
    hex[0] = 0;
    if (psa_crypto_init() != PSA_SUCCESS || psa_hash_compute(PSA_ALG_SHA_256, key.data(), key.size(),
                                                             hash.data(), hash.size(), &n) != PSA_SUCCESS)
        return false;
    constexpr char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8; ++i) {
        hex[2 * i] = digits[hash[i] >> 4];
        hex[2 * i + 1] = digits[hash[i] & 15];
    }
    hex[16] = 0;
    clear_secret(hash.data(), hash.size());
    return true;
}
} // namespace goldrop
