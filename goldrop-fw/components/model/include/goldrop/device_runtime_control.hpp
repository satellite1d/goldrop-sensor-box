#pragma once
#include <cstdint>
namespace goldrop {
class DeviceRuntimeControl {
  public:
    virtual ~DeviceRuntimeControl() = default;
    virtual bool set_sampling_interval(uint32_t interval_ms) = 0;
    virtual bool request_firmware_check() = 0;
};
} // namespace goldrop
