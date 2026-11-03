#pragma once
#include "goldrop/measurement.hpp"

namespace goldrop {
enum class SendResult { Accepted, Offline, Busy, Failed };
class TelemetryTransport {
public:
    virtual ~TelemetryTransport() = default;
    virtual bool connected() const = 0;
    // Bounded enqueue only. Accepted is not proof of backend delivery.
    virtual SendResult try_send(const Measurement& measurement) = 0;
};
}  // namespace goldrop
