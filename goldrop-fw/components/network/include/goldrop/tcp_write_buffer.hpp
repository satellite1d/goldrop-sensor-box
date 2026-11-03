#pragma once
#include "goldrop/serialization.hpp"
#include "goldrop/settings.hpp"
#include <cstring>
#include <string_view>

namespace goldrop {
// One bounded NDJSON frame. A receipt appears only after every byte was written.
class TcpWriteBuffer {
  public:
    bool prepare_status(std::string_view line) {
        if (busy() || line.empty() || line.size() >= sizeof(bytes_) - 1 ||
            line.find_first_of("\r\n") != line.npos || line.find('\0') != line.npos)
            return false;
        std::memcpy(bytes_, line.data(), line.size());
        bytes_[line.size()] = '\n';
        size_ = line.size() + 1;
        offset_ = 0;
        return true;
    }
    bool prepare(const Measurement &measurement) {
        if (busy() || !serialize_measurement(measurement, bytes_, sizeof(bytes_)))
            return false;
        const std::size_t length = std::strlen(bytes_);
        if (length >= sizeof(bytes_) - 1)
            return false;
        bytes_[length] = '\n';
        size_ = length + 1;
        offset_ = 0;
        measurement_ = measurement;
        return true;
    }
    bool busy() const { return size_ != 0; }
    const char *data() const { return bytes_ + offset_; }
    std::size_t remaining() const { return size_ - offset_; }
    bool advance(std::size_t count) {
        if (count > remaining())
            return false;
        offset_ += count;
        return true;
    }
    bool take_written(Measurement &measurement) {
        if (!busy() || remaining() != 0)
            return false;
        measurement = measurement_;
        reset();
        return true;
    }
    void reset() { size_ = offset_ = 0; }

  private:
    char bytes_[settings::kJsonBufferSize]{};
    Measurement measurement_{};
    std::size_t size_{}, offset_{};
};
} // namespace goldrop
