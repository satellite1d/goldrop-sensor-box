#include "goldrop/telemetry_service.hpp"

namespace goldrop {
void TelemetryService::poll(int64_t now_ms) {
    if (!transport_.connected()) return;
    if (awaiting_ack_) {
        if (now_ms - last_send_ms_ < ack_timeout_ms_) return;
    } else if (!store_.peek(pending_)) {
        return;
    }
    if (transport_.try_send(pending_) == SendResult::Accepted) {
        awaiting_ack_ = true;
        last_send_ms_ = now_ms;
    }
}

bool TelemetryService::acknowledge(const Measurement& measurement, DeliveryConfirmation source) {
    if (source != confirmation_ || !awaiting_ack_ || !same_measurement(pending_, measurement)) return false;
    // The pending record may have been evicted on overflow. Never remove its replacement.
    store_.acknowledge(pending_);
    awaiting_ack_ = false;
    return true;
}
}  // namespace goldrop
