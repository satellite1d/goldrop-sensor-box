#include "goldrop/usb_wifi_console.hpp"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "goldrop/firmware_identity.hpp"
#include "goldrop/management_crypto.hpp"
#include <cinttypes>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>

namespace goldrop {
namespace {
constexpr const char *kTag = "usb_config";
}
UsbWifiConsole::~UsbWifiConsole() {
    if (!owns_driver_)
        return;
    usb_serial_jtag_vfs_use_nonblocking();
    const esp_err_t result = usb_serial_jtag_driver_uninstall();
    if (result != ESP_OK)
        ESP_LOGW(kTag, "USB driver cleanup failed: %s", esp_err_to_name(result));
}
bool UsbWifiConsole::begin() {
    if (ready_)
        return true;
    const int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (flags < 0 || fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK) < 0) {
        ESP_LOGE(kTag, "Nonblocking USB input unavailable, errno=%d", errno);
        return false;
    }
    // IDF 6.1 VFS nonblocking read checks the driver's RX ring buffer, even
    // when startup still uses the basic FIFO backend. Install it before polling.
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config{};
        config.rx_buffer_size = 512;
        config.tx_buffer_size = 1024;
        const esp_err_t result = usb_serial_jtag_driver_install(&config);
        if (result != ESP_OK) {
            ESP_LOGE(kTag, "USB driver initialization failed: %s", esp_err_to_name(result));
            return false;
        }
        owns_driver_ = true;
    }
    usb_serial_jtag_vfs_use_driver();
    // The protocol handles both CR and LF itself, including fragmented CRLF.
    usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_LF);
    ready_ = true;
    ESP_LOGI(kTag, "CFG READY (wifi help); input is not echoed");
    ESP_LOGI(kTag, "Konfiguracja na komputerze: tools/provision_wifi.py --port <port USB>");
    return true;
}
void UsbWifiConsole::poll(int64_t now) {
    if (!ready_)
        return;
    reply(protocol_.tick(now));
    char bytes[128];
    const ssize_t received = read(STDIN_FILENO, bytes, sizeof(bytes));
    if (received < 0 && errno == EIO)
        protocol_.cancel(); // USB detached: discard staged secrets.
    if (received > 0) {
        for (ssize_t i = 0; i < received; ++i)
            reply(protocol_.feed(bytes[i], now));
    }
    clear_secret(bytes, sizeof(bytes));
}
void UsbWifiConsole::reply(ProvisionReply result) {
    // Never echo command lines, SSIDs or passwords. Replies have a stable CFG prefix.
    switch (result) {
    case ProvisionReply::ManageField:
        ESP_LOGI(kTag, "CFG OK MANAGE FIELD");
        break;
    case ProvisionReply::ConfigSaved:
        ESP_LOGI(kTag, "CFG OK CONFIG");
        break;
    case ProvisionReply::ConfigBusy:
        ESP_LOGW(kTag, "CFG ERROR BUSY (OTA or boot validation)");
        break;
    case ProvisionReply::ConfigFailed:
        ESP_LOGE(kTag,
                 "CFG ERROR CONFIG (operation failed; inspect manage status, recover via USB if needed)");
        break;
    case ProvisionReply::ManageStatus: {
        if (!config_)
            break;
        const auto &s = config_->config_state();
        char mac[18], fingerprint[17]{};
        const bool has_key = s.management.enrolled && management_key_id(s.management.key, fingerprint);
        ESP_LOGI(kTag,
                 "CFG MANAGE mac=%s healthy=%s busy=%s enrolled=%s enabled=%s key_id=%s phase=%s "
                 "active_revision=%" PRIu64 " highwater=%" PRIu64 " pending_revision=%" PRIu64
                 " report_sequence=%" PRIu64 " result=%s error=%s",
                 config_->configuration_device().format(mac) ? mac : "none",
                 config_->configuration_healthy() ? "true" : "false",
                 config_->configuration_busy() ? "true" : "false", s.management.enrolled ? "true" : "false",
                 s.management.enabled ? "true" : "false", has_key ? fingerprint : "none",
                 config_phase_name(s.phase), s.active_revision, s.highwater, s.pending_revision,
                 s.report_sequence, config_result_name(s.result), config_error_name(s.error));
        break;
    }
    case ProvisionReply::None:
        return;
    case ProvisionReply::Ssid:
        ESP_LOGI(kTag, "CFG OK SSID");
        break;
    case ProvisionReply::Password:
        ESP_LOGI(kTag, "CFG OK PASSWORD");
        break;
    case ProvisionReply::Saved:
        ESP_LOGI(kTag, "CFG OK SAVE (NVS saved; connecting asynchronously)");
        break;
    case ProvisionReply::Forgotten:
        ESP_LOGI(kTag, "CFG OK FORGET");
        break;
    case ProvisionReply::OtaField:
        ESP_LOGI(kTag, "CFG OK OTA FIELD");
        break;
    case ProvisionReply::OtaSaved:
        ESP_LOGI(kTag, "CFG OK OTA SAVE");
        break;
    case ProvisionReply::OtaDisabled:
        ESP_LOGI(kTag, "CFG OK OTA DISABLE");
        break;
    case ProvisionReply::OtaForgotten:
        ESP_LOGI(kTag, "CFG OK OTA FORGET");
        break;
    case ProvisionReply::OtaCheck:
        ESP_LOGI(kTag, "CFG OK OTA CHECK (request accepted; use ota status)");
        break;
    case ProvisionReply::OtaRetry:
        ESP_LOGI(kTag, "CFG OK OTA RETRY (release block cleared)");
        break;
    case ProvisionReply::OtaRejected:
        ESP_LOGW(kTag, "CFG ERROR OTA_NOT_READY (missing config/public key, busy or storage error)");
        break;
    case ProvisionReply::OtaStatus:
        ESP_LOGI(kTag,
                 "CFG OTA state=%s progress=%u error=%s firmware=%s hardware=%s signed_http=%s public_key=%s",
                 ota_ ? ota_state_name(ota_->state()) : "not_configured", ota_ ? ota_->progress() : 0,
                 ota_ ? ota_->last_error() : "none", kFirmwareVersion, kHardwareRevision,
                 ota_ && ota_->signed_http_supported() ? "true" : "false",
                 ota_ ? ota_->public_key_id() : "none");
        break;
    case ProvisionReply::ServerAddress:
        ESP_LOGI(kTag, "CFG OK SERVER ADDRESS");
        break;
    case ProvisionReply::ServerPort:
        ESP_LOGI(kTag, "CFG OK SERVER PORT");
        break;
    case ProvisionReply::ServerSaved:
        ESP_LOGI(kTag, "CFG OK SERVER SAVE (NVS saved; test TCP)");
        break;
    case ProvisionReply::ServerForgotten:
        ESP_LOGI(kTag, "CFG OK SERVER FORGET");
        break;
    case ProvisionReply::ServerStatus:
        ESP_LOGI(kTag, "CFG SERVER state=%s transport=tcp_test",
                 server_ ? server_state_name(server_->server_state()) : "unconfigured");
        break;
    case ProvisionReply::Status: {
        const NetworkStatus status = network_.status();
        ESP_LOGI(kTag, "CFG STATUS state=%s ip=%s rssi_dbm=%d rssi_valid=%s", wifi_state_name(status.state),
                 status.ip, status.rssi_dbm, status.rssi_valid ? "true" : "false");
        break;
    }
    case ProvisionReply::Help:
        ESP_LOGI(
            kTag,
            "CFG HELP wifi ssid <value> | wifi password <value> | wifi save | wifi status | wifi forget");
        ESP_LOGI(kTag, "CFG HELP server address <DNS-or-IPv4> | server port <1..65535> | server save | "
                       "server status | server forget");
        ESP_LOGI(
            kTag,
            "CFG HELP ota url <https://...|http://...> | ota channel <name> | ota interval <60..86400> | "
            "ota enable <0|1> | ota save");
        ESP_LOGI(kTag, "CFG HELP ota status | ota check | ota retry | ota disable | ota forget");
        ESP_LOGI(kTag, "CFG HELP manage url <endpoint> | manage key <64hex> | manage save | manage status | "
                       "manage enable <0|1> | manage forget");
        ESP_LOGI(kTag,
                 "CFG HELP config cancel | config recover (USB recovery clears management key/counters)");
        break;
    case ProvisionReply::Invalid:
        ESP_LOGW(kTag, "CFG ERROR INVALID (use wifi help for Wi-Fi, server and OTA commands)");
        break;
    case ProvisionReply::TooLong:
        ESP_LOGW(kTag, "CFG ERROR TOO_LONG");
        break;
    case ProvisionReply::Timeout:
        ESP_LOGW(kTag, "CFG ERROR TIMEOUT (staged input discarded)");
        break;
    case ProvisionReply::StorageError:
        ESP_LOGE(kTag, "CFG ERROR STORAGE (active configuration unchanged)");
        break;
    case ProvisionReply::ApplyError:
        ESP_LOGE(kTag, "CFG ERROR APPLY_SAVED (NVS saved; restart to apply)");
        break;
    }
}
} // namespace goldrop
