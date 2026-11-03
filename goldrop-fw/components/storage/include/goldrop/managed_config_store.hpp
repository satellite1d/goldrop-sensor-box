#pragma once
#include "goldrop/managed_config.hpp"
#include "goldrop/config_store.hpp"
namespace goldrop {
using ManagedConfigBlob = std::array<uint8_t, 1280>;
bool encode_managed_config(const ManagedConfigState &, ManagedConfigBlob &);
bool decode_managed_config(const uint8_t *, std::size_t, ManagedConfigState &);
class ManagedConfigStore {
  public:
    virtual ~ManagedConfigStore() = default;
    virtual ConfigLoadResult load(ManagedConfigState &) = 0;
    virtual bool save(const ManagedConfigState &) = 0;
};
class NvsManagedConfigStore final : public ManagedConfigStore {
  public:
    ConfigLoadResult load(ManagedConfigState &) override;
    bool save(const ManagedConfigState &) override;
};
} // namespace goldrop
