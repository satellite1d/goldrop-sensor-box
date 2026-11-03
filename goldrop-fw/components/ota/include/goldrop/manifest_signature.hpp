#pragma once
#include "goldrop/ota_manifest.hpp"
#include "mbedtls/pk.h"
namespace goldrop {
class MbedtlsManifestVerifier final : public ManifestSignatureVerifier {
  public:
    MbedtlsManifestVerifier() { mbedtls_pk_init(&key_); }
    ~MbedtlsManifestVerifier() override { mbedtls_pk_free(&key_); }
    MbedtlsManifestVerifier(const MbedtlsManifestVerifier &) = delete;
    MbedtlsManifestVerifier &operator=(const MbedtlsManifestVerifier &) = delete;
    bool begin();
    bool available() const override { return ready_; }
    bool verify(std::string_view, const uint8_t *, std::size_t) override;
    const char *key_id() const { return ready_ ? fingerprint_ : "none"; }

  private:
    mbedtls_pk_context key_;
    bool ready_{};
    char fingerprint_[65]{};
};
} // namespace goldrop
