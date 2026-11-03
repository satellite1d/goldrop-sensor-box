#pragma once
#include "goldrop/config_store.hpp"
#include "goldrop/ota_config.hpp"
namespace goldrop {
using OtaConfigBlob = std::array<uint8_t, 416>;
bool encode_ota_config(const OtaConfig &, OtaConfigBlob &);
bool decode_ota_config(const uint8_t *, std::size_t, OtaConfig &);
class OtaConfigStore {
  public:
    virtual ~OtaConfigStore() = default;
    virtual ConfigLoadResult load_ota(OtaConfig &) = 0;
    virtual bool save_ota(const OtaConfig &) = 0;
    virtual bool forget_ota() = 0;
};
class NvsOtaConfigStore final : public OtaConfigStore {
  public:
    ConfigLoadResult load_ota(OtaConfig &) override;
    bool save_ota(const OtaConfig &) override;
    bool forget_ota() override;
};
struct OtaAttempt {
    std::array<uint8_t, 32> hash{};
    char version[32]{};
    bool pending{};
    bool blocked{};
};
class OtaAttemptStore {
  public:
    bool load(OtaAttempt &);
    bool save(const OtaAttempt &);
    bool clear();
};
} // namespace goldrop
