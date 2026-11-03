#include "goldrop/wifi_provisioner.hpp"
#include <string_view>

namespace goldrop {
void WifiProvisioner::clear_line() {
    clear_secret(line_, sizeof(line_));
    length_ = 0;
}
void WifiProvisioner::cancel() {
    clear_line();
    staged_.clear();
    ssid_set_ = password_set_ = discarding_ = false;
    staged_server_ = {};
    server_address_set_ = server_port_set_ = false;
    staged_ota_ = {};
    ota_staged_ = false;
    staged_management_.clear();
    management_url_set_ = management_key_set_ = false;
}
ProvisionReply WifiProvisioner::tick(int64_t now) {
    if (length_ && now - last_input_ms_ >= 5'000) {
        cancel();
        discarding_ = true; // Never interpret the tail of an abandoned password as a command.
        return ProvisionReply::Timeout;
    }
    if ((ssid_set_ || password_set_ || server_address_set_ || server_port_set_ || ota_staged_ ||
         management_url_set_ || management_key_set_) &&
        now - last_input_ms_ >= 300'000) {
        cancel();
        return ProvisionReply::Timeout;
    }
    return ProvisionReply::None;
}
ProvisionReply WifiProvisioner::feed(char byte, int64_t now) {
    last_input_ms_ = now;
    if (byte == '\r' || byte == '\n') {
        if (discarding_) {
            discarding_ = false;
            clear_line();
            return ProvisionReply::None;
        }
        if (!length_)
            return ProvisionReply::None;
        const ProvisionReply reply = execute(now);
        clear_line();
        return reply;
    }
    if (discarding_)
        return ProvisionReply::None;
    if (byte == '\b' || byte == 0x7F) {
        if (length_)
            line_[--length_] = 0;
        return ProvisionReply::None;
    }
    if (static_cast<unsigned char>(byte) < 0x20) {
        cancel();
        discarding_ = true;
        return ProvisionReply::Invalid;
    }
    if (length_ == sizeof(line_) - 1) {
        cancel();
        discarding_ = true;
        return ProvisionReply::TooLong;
    }
    line_[length_++] = byte;
    return ProvisionReply::None;
}

ProvisionReply WifiProvisioner::prepare_local_change(int64_t now) {
    if (!config_)
        return ProvisionReply::None;
    const auto result = config_->configuration_busy()  ? ProvisionReply::ConfigBusy
                        : config_->local_override(now) ? ProvisionReply::None
                                                       : ProvisionReply::ConfigFailed;
    if (result != ProvisionReply::None)
        cancel();
    return result;
}
ProvisionReply WifiProvisioner::execute(int64_t now) {
    const std::string_view line(line_, length_);
    constexpr std::string_view ssid_prefix = "wifi ssid ";
    constexpr std::string_view password_prefix = "wifi password ";
    if (config_ && (line.substr(0, 7) == "manage " || line.substr(0, 7) == "config ")) {
        if (line == "manage status" || line == "config status")
            return ProvisionReply::ManageStatus;
        if (config_->configuration_busy())
            return ProvisionReply::ConfigBusy;
        if (line.substr(0, 11) == "manage url ") {
            staged_management_.clear();
            management_url_set_ = management_key_set_ = false;
            if (!staged_management_.set_endpoint(line.substr(11)))
                return ProvisionReply::Invalid;
            const auto prepared = prepare_local_change(now);
            if (prepared != ProvisionReply::None)
                return prepared;
            management_url_set_ = true;
            return ProvisionReply::ManageField;
        }
        if (line.substr(0, 11) == "manage key ") {
            management_key_set_ = false;
            staged_management_.key.fill(0);
            const auto value = line.substr(11);
            if (value.size() != 64)
                return ProvisionReply::Invalid;
            for (unsigned i = 0; i < 32; ++i) {
                const auto digit = [](char c) -> int {
                    return c >= '0' && c <= '9'   ? c - '0'
                           : c >= 'a' && c <= 'f' ? c - 'a' + 10
                           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                  : -1;
                };
                const int a = digit(value[2 * i]), b = digit(value[2 * i + 1]);
                if (a < 0 || b < 0) {
                    staged_management_.key.fill(0);
                    return ProvisionReply::Invalid;
                }
                staged_management_.key[i] = uint8_t((a << 4) | b);
            }
            management_key_set_ = staged_management_.key != std::array<uint8_t, 32>{};
            return management_key_set_ ? ProvisionReply::ManageField : ProvisionReply::Invalid;
        }
        bool ok = false;
        if (line == "manage save") {
            staged_management_.enrolled = staged_management_.enabled = true;
            if (!management_url_set_ || !management_key_set_ || !staged_management_.valid())
                return ProvisionReply::Invalid;
            ok = config_->enroll_management(staged_management_, now);
            cancel();
        } else if (line == "manage enable 0" || line == "manage enable 1")
            ok = config_->enable_management(line.back() == '1', now);
        else if (line == "manage forget")
            ok = config_->forget_management(now);
        else if (line == "config cancel")
            ok = config_->local_override(now);
        else if (line == "config recover") {
            cancel();
            ok = config_->recover_configuration(now);
        } else
            return ProvisionReply::Invalid;
        return ok ? ProvisionReply::ConfigSaved : ProvisionReply::ConfigFailed;
    }
    if (line.substr(0, 4) == "ota " && ota_store_ && ota_) {
        if (line.substr(0, 8) == "ota url ") {
            ota_staged_ = false;
            if (!staged_ota_.set_url(line.substr(8)))
                return ProvisionReply::Invalid;
            ota_staged_ = true;
            return ProvisionReply::OtaField;
        }
        if (line.substr(0, 12) == "ota channel ") {
            if (!staged_ota_.set_channel(line.substr(12))) {
                ota_staged_ = false;
                return ProvisionReply::Invalid;
            }
            return ProvisionReply::OtaField;
        }
        if (line.substr(0, 13) == "ota interval ") {
            uint32_t value = 0;
            const auto text = line.substr(13);
            if (text.empty() || text.size() > 5) {
                ota_staged_ = false;
                return ProvisionReply::Invalid;
            }
            for (char c : text) {
                if (c < '0' || c > '9') {
                    ota_staged_ = false;
                    return ProvisionReply::Invalid;
                }
                value = value * 10 + uint32_t(c - '0');
            }
            if (value < 60 || value > 86400) {
                ota_staged_ = false;
                return ProvisionReply::Invalid;
            }
            staged_ota_.interval_seconds = value;
            return ProvisionReply::OtaField;
        }
        if (line == "ota enable 0" || line == "ota enable 1") {
            staged_ota_.enabled = line.back() == '1';
            return ProvisionReply::OtaField;
        }
        if (line == "ota save") {
            if (!ota_staged_ || !staged_ota_.valid())
                return ProvisionReply::Invalid;
            HttpUrl endpoint;
            if (!endpoint.parse(staged_ota_.manifest_url.data()) ||
                (!endpoint.tls && !ota_->signed_http_supported()))
                return ProvisionReply::OtaRejected;
            if (!ota_store_->save_ota(staged_ota_))
                return ProvisionReply::StorageError;
            const bool accepted = ota_->configure_ota(staged_ota_, now);
            staged_ota_ = {};
            ota_staged_ = false;
            return accepted ? ProvisionReply::OtaSaved : ProvisionReply::ApplyError;
        }
        if (line == "ota disable") {
            OtaConfig config;
            const auto loaded = ota_store_->load_ota(config);
            if (loaded == ConfigLoadResult::Failed)
                return ProvisionReply::StorageError;
            if (loaded == ConfigLoadResult::Loaded) {
                config.enabled = false;
                if (!ota_store_->save_ota(config))
                    return ProvisionReply::StorageError;
                if (!ota_->configure_ota(config, now))
                    return ProvisionReply::ApplyError;
            } else
                ota_->disable_ota();
            staged_ota_ = {};
            ota_staged_ = false;
            return ProvisionReply::OtaDisabled;
        }
        if (line == "ota forget") {
            if (!ota_store_->forget_ota())
                return ProvisionReply::StorageError;
            ota_->disable_ota();
            staged_ota_ = {};
            ota_staged_ = false;
            return ProvisionReply::OtaForgotten;
        }
        if (line == "ota status")
            return ProvisionReply::OtaStatus;
        if (line == "ota check")
            return ota_->request_check() ? ProvisionReply::OtaCheck : ProvisionReply::OtaRejected;
        if (line == "ota retry")
            return ota_->clear_block() ? ProvisionReply::OtaRetry : ProvisionReply::OtaRejected;
        ota_staged_ = false;
        return ProvisionReply::Invalid;
    }
    if (line.substr(0, 7) == "server " && server_store_ && server_) {
        constexpr std::string_view address_prefix = "server address ";
        constexpr std::string_view port_prefix = "server port ";
        if (line.substr(0, address_prefix.size()) == address_prefix) {
            const auto value = line.substr(address_prefix.size());
            server_address_set_ = false;
            if (!staged_server_.set_address(value.data(), value.size()))
                return ProvisionReply::Invalid;
            const auto prepared = prepare_local_change(now);
            if (prepared != ProvisionReply::None)
                return prepared;
            server_address_set_ = true;
            return ProvisionReply::ServerAddress;
        }
        if (line.substr(0, port_prefix.size()) == port_prefix) {
            const auto value = line.substr(port_prefix.size());
            server_port_set_ = false;
            if (!staged_server_.set_port(value.data(), value.size()))
                return ProvisionReply::Invalid;
            const auto prepared = prepare_local_change(now);
            if (prepared != ProvisionReply::None)
                return prepared;
            server_port_set_ = true;
            return ProvisionReply::ServerPort;
        }
        if (line == "server save") {
            if (!server_address_set_ || !server_port_set_ || !staged_server_.valid())
                return ProvisionReply::Invalid;
            const auto prepared = prepare_local_change(now);
            if (prepared != ProvisionReply::None)
                return prepared;
            if (!config_ && !server_store_->save_server(staged_server_))
                return ProvisionReply::StorageError;
            const bool accepted = config_ ? config_->local_server(staged_server_, now)
                                          : server_->configure_server(staged_server_, now);
            staged_server_ = {};
            server_address_set_ = server_port_set_ = false;
            if (config_ && !accepted)
                return ProvisionReply::ConfigFailed;
            return accepted ? ProvisionReply::ServerSaved : ProvisionReply::ApplyError;
        }
        if (line == "server forget") {
            const auto prepared = prepare_local_change(now);
            if (prepared != ProvisionReply::None)
                return prepared;
            if (config_ ? !config_->local_forget_server(now) : !server_store_->forget_server())
                return config_ ? ProvisionReply::ConfigFailed : ProvisionReply::StorageError;
            if (!config_)
                server_->disable_server();
            staged_server_ = {};
            server_address_set_ = server_port_set_ = false;
            return ProvisionReply::ServerForgotten;
        }
        if (line == "server status")
            return ProvisionReply::ServerStatus;
        return ProvisionReply::Invalid;
    }
    if (line.substr(0, ssid_prefix.size()) == ssid_prefix) {
        const auto value = line.substr(ssid_prefix.size());
        ssid_set_ = false;
        if (!staged_.set_ssid(value.data(), value.size()))
            return ProvisionReply::Invalid;
        const auto prepared = prepare_local_change(now);
        if (prepared != ProvisionReply::None)
            return prepared;
        ssid_set_ = true;
        password_set_ = false;
        clear_secret(staged_.password.data(), staged_.password.size());
        return ProvisionReply::Ssid;
    }
    if (line.substr(0, password_prefix.size()) == password_prefix) {
        const auto value = line.substr(password_prefix.size());
        password_set_ = false;
        if (!staged_.set_password(value.data(), value.size()))
            return ProvisionReply::Invalid;
        const auto prepared = prepare_local_change(now);
        if (prepared != ProvisionReply::None)
            return prepared;
        password_set_ = true;
        return ProvisionReply::Password;
    }
    if (line == "wifi save") {
        if (!ssid_set_ || !password_set_ || !staged_.valid())
            return ProvisionReply::Invalid;
        const auto prepared = prepare_local_change(now);
        if (prepared != ProvisionReply::None)
            return prepared;
        if (!config_ && !store_.save_wifi(staged_))
            return ProvisionReply::StorageError;
        const bool accepted = config_ ? config_->local_wifi(staged_, now) : network_.configure(staged_, now);
        cancel();
        if (config_ && !accepted)
            return ProvisionReply::ConfigFailed;
        return accepted ? ProvisionReply::Saved : ProvisionReply::ApplyError;
    }
    if (line == "wifi forget") {
        const auto prepared = prepare_local_change(now);
        if (prepared != ProvisionReply::None)
            return prepared;
        if (config_ ? !config_->local_forget_wifi(now) : !store_.forget_wifi())
            return config_ ? ProvisionReply::ConfigFailed : ProvisionReply::StorageError;
        if (!config_)
            network_.disable(now);
        cancel();
        return ProvisionReply::Forgotten;
    }
    if (line == "wifi status")
        return ProvisionReply::Status;
    if (line == "wifi help")
        return ProvisionReply::Help;
    return ProvisionReply::Invalid;
}
} // namespace goldrop
