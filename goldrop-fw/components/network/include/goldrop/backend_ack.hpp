#pragma once
#include "goldrop/measurement.hpp"
#include <array>
#include <string_view>
namespace goldrop {
// ACK1 <station-mac> <boot_id> <sequence> LF. Fixed storage, no heap allocations.
bool parse_backend_ack(std::string_view, Measurement &);
class BackendAckReader {
  public:
    bool feed(char byte, int64_t now, Measurement &);
    void poll(int64_t now);
    void reset() {
        bytes_.fill(0);
        size_ = 0;
        discarding_ = false;
        started_ = 0;
    }

  private:
    std::array<char, 96> bytes_{};
    std::size_t size_{};
    bool discarding_{};
    int64_t started_{};
};
} // namespace goldrop
