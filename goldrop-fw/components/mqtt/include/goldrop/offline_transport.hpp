#pragma once
#include "goldrop/telemetry_transport.hpp"

namespace goldrop {
// Intentionally offline until credentials and the TLS MQTT adapter exist.
class OfflineTransport final : public TelemetryTransport {
public:
    bool connected() const override { return false; }
    SendResult try_send(const Measurement&) override { return SendResult::Offline; }
};
}  // namespace goldrop
