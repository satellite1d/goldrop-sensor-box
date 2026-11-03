#pragma once
#include "goldrop/measurement_store.hpp"
#include "goldrop/telemetry_transport.hpp"
#include "goldrop/settings.hpp"

namespace goldrop {
enum class DeliveryConfirmation { BackendAck, TcpWrite };
class TelemetryService {
public:
    TelemetryService(MeasurementStore& store, TelemetryTransport& transport,
                     int64_t ack_timeout_ms = settings::kAckTimeoutMs,
                     DeliveryConfirmation confirmation = DeliveryConfirmation::BackendAck)
        : store_(store), transport_(transport), ack_timeout_ms_(ack_timeout_ms), confirmation_(confirmation) {}
    AppendResult ingest(const Measurement& measurement) { return store_.append(measurement); }
    void poll(int64_t now_ms);
    // Backend ACK remains the default. Test TCP explicitly opts into socket-write receipts.
    bool acknowledge(const Measurement& measurement,
                     DeliveryConfirmation source = DeliveryConfirmation::BackendAck);
private:
    MeasurementStore& store_;
    TelemetryTransport& transport_;
    int64_t ack_timeout_ms_;
    DeliveryConfirmation confirmation_;
    Measurement pending_{};
    bool awaiting_ack_{};
    int64_t last_send_ms_{};
};
}  // namespace goldrop
