#include "goldrop/manifest_signature.hpp"
#include "ota_verification_key.hpp"
#include "esp_log.h"
#include "psa/crypto.h"
namespace goldrop {
bool MbedtlsManifestVerifier::begin() {
    if (ready_)
        return true;
    if (sizeof(kOtaVerificationKey) <= 1)
        return false;
    if (psa_crypto_init() != PSA_SUCCESS ||
        mbedtls_pk_parse_public_key(&key_, reinterpret_cast<const uint8_t *>(kOtaVerificationKey),
                                    sizeof(kOtaVerificationKey)) != 0 ||
        mbedtls_pk_get_bitlen(&key_) != 256 ||
        !mbedtls_pk_can_do_psa(&key_, PSA_ALG_ECDSA(PSA_ALG_SHA_256), PSA_KEY_USAGE_VERIFY_HASH)) {
        ESP_LOGE("ota", "Invalid embedded OTA public key; signed HTTP unavailable");
        return false;
    }
    std::array<uint8_t, 32> hash{};
    std::size_t size{};
    if (psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t *>(kOtaVerificationKey),
                         sizeof(kOtaVerificationKey) - 1, hash.data(), hash.size(), &size) != PSA_SUCCESS ||
        size != hash.size())
        return false;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < hash.size(); ++i) {
        fingerprint_[2 * i] = hex[hash[i] >> 4];
        fingerprint_[2 * i + 1] = hex[hash[i] & 15];
    }
    ready_ = true;
    ESP_LOGI("ota", "Signed HTTP enabled; public key SHA-256=%s", fingerprint_);
    return true;
}
bool MbedtlsManifestVerifier::verify(std::string_view payload, const uint8_t *signature, std::size_t size) {
    if (!ready_ || !signature || size < 8 || size > 72 || payload.empty() || payload.size() > kManifestLimit)
        return false;
    std::array<uint8_t, 32> hash{};
    std::size_t hash_size{};
    if (psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t *>(payload.data()), payload.size(),
                         hash.data(), hash.size(), &hash_size) != PSA_SUCCESS ||
        hash_size != hash.size())
        return false;
    return mbedtls_pk_verify(&key_, MBEDTLS_MD_SHA256, hash.data(), hash.size(), signature, size) == 0;
}
} // namespace goldrop
