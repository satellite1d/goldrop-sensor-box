#pragma once
#include "goldrop/settings.hpp"

namespace goldrop {
struct SamplingConfig {
    uint32_t interval_ms{settings::kSamplingIntervalMs};
    bool valid() const {
        return interval_ms >= settings::kMinimumSamplingIntervalMs &&
               interval_ms <= settings::kMaximumSamplingIntervalMs;
    }
};
enum class ConfigLoadResult { Loaded, Defaults, Failed };
class ConfigStore {
public:
    virtual ~ConfigStore() = default;
    virtual ConfigLoadResult load(SamplingConfig& result) = 0;
};

// Bootstrap implementation. No persistent format or credentials introduced yet.
class DefaultConfigStore final : public ConfigStore {
public:
    ConfigLoadResult load(SamplingConfig& result) override {
        result = SamplingConfig{};
        return ConfigLoadResult::Defaults;
    }
};
}  // namespace goldrop
