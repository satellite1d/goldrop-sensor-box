#pragma once
#include "goldrop/wifi_provisioner.hpp"

namespace goldrop {
class UsbWifiConsole {
  public:
    UsbWifiConsole(WifiConfigStore &store, NetworkControl &network, ServerConfigStore *server_store = nullptr,
                   ServerControl *server = nullptr, OtaConfigStore *ota_store = nullptr,
                   OtaService *ota = nullptr, ConfigurationControl *config = nullptr)
        : protocol_(store, network, server_store, server, ota_store, ota, config), network_(network),
          server_(server), ota_(ota), config_(config) {}
    ~UsbWifiConsole();
    UsbWifiConsole(const UsbWifiConsole &) = delete;
    UsbWifiConsole &operator=(const UsbWifiConsole &) = delete;
    bool begin();
    void poll(int64_t now_ms);

  private:
    void reply(ProvisionReply result);
    WifiProvisioner protocol_;
    NetworkControl &network_;
    ServerControl *server_;
    OtaService *ota_;
    ConfigurationControl *config_;
    bool ready_{};
    bool owns_driver_{};
};
} // namespace goldrop
