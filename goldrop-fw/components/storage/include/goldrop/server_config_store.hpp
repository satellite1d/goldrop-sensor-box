#pragma once
#include "goldrop/config_store.hpp"
#include "goldrop/server_config.hpp"
#include <array>

namespace goldrop {
using ServerConfigBlob = std::array<uint8_t, 264>;
using LegacyServerConfigBlob = std::array<uint8_t, 12>;
bool encode_server_config(const ServerConfig& config, ServerConfigBlob& blob);
bool decode_server_config(const uint8_t* bytes, std::size_t size, ServerConfig& config);
class ServerConfigStore {
public:
    virtual ~ServerConfigStore() = default;
    virtual ConfigLoadResult load_server(ServerConfig& result) = 0;
    virtual bool save_server(const ServerConfig& config) = 0;
    virtual bool forget_server() = 0;
};
class NvsServerConfigStore final : public ServerConfigStore {
public:
    ConfigLoadResult load_server(ServerConfig& result) override;
    bool save_server(const ServerConfig& config) override;
    bool forget_server() override;
};
}  // namespace goldrop
