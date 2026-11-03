#pragma once
#include "esp_err.h"
#include <cstdint>

namespace goldrop {
class BootValidation {
  public:
    esp_err_t begin();
    void poll(bool core_initialized, uint32_t acquired, uint32_t buffered, int64_t elapsed_ms);
    bool ready() const { return !pending_; }

  private:
    bool pending_{};
    unsigned confirm_attempts_{};
    int64_t next_attempt_ms_{};
};
} // namespace goldrop
