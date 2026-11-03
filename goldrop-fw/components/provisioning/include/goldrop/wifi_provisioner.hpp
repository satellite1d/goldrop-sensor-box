#pragma once
#include "goldrop/network_control.hpp"
#include "goldrop/wifi_config_store.hpp"
#include "goldrop/server_config_store.hpp"
#include "goldrop/server_control.hpp"
#include "goldrop/ota_service.hpp"
#include "goldrop/ota_config_store.hpp"
#include "goldrop/managed_config.hpp"

namespace goldrop {
enum class ProvisionReply {
    None,
    Ssid,
    Password,
    Saved,
    Forgotten,
    Status,
    Help,
    Invalid,
    TooLong,
    Timeout,
    StorageError,
    ApplyError,
    ServerAddress,
    ServerPort,
    ServerSaved,
    ServerForgotten,
    ServerStatus,
    OtaField,
    OtaSaved,
    OtaDisabled,
    OtaForgotten,
    OtaStatus,
    OtaCheck,
    OtaRetry,
    OtaRejected,
    ManageField,
    ConfigSaved,
    ManageStatus,
    ConfigBusy,
    ConfigFailed
};
class WifiProvisioner {
  public:
    WifiProvisioner(WifiConfigStore &store, NetworkControl &network,
                    ServerConfigStore *server_store = nullptr, ServerControl *server = nullptr,
                    OtaConfigStore *ota_store = nullptr, OtaService *ota = nullptr,
                    ConfigurationControl *config = nullptr)
        : store_(store), network_(network), server_store_(server_store), server_(server),
          ota_store_(ota_store), ota_(ota), config_(config) {}
    ~WifiProvisioner() { cancel(); }
    ProvisionReply feed(char byte, int64_t now_ms);
    ProvisionReply tick(int64_t now_ms);
    void cancel();

  private:
    ProvisionReply execute(int64_t now_ms);
    ProvisionReply prepare_local_change(int64_t now_ms);
    void clear_line();
    WifiConfigStore &store_;
    NetworkControl &network_;
    ServerConfigStore *server_store_;
    ServerControl *server_;
    OtaConfigStore *ota_store_;
    OtaService *ota_;
    ConfigurationControl *config_;
    ManagementEnrollment staged_management_;
    bool management_url_set_{}, management_key_set_{};
    OtaConfig staged_ota_;
    bool ota_staged_{};
    ServerConfig staged_server_;
    WifiCredentials staged_;
    char line_[settings::kProvisionLineCapacity]{};
    std::size_t length_{};
    int64_t last_input_ms_{};
    bool ssid_set_{};
    bool password_set_{};
    bool discarding_{};
    bool server_address_set_{}, server_port_set_{};
};
} // namespace goldrop
