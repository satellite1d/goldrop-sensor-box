#pragma once
#include "goldrop/config_store.hpp"
namespace goldrop {
class NvsSamplingConfigStore final : public ConfigStore {
  public:
    ConfigLoadResult load(SamplingConfig &) override;
    bool save(uint32_t interval_ms);
};
} // namespace goldrop
