#pragma once
#include "goldrop/ota_manifest.hpp"
#include <algorithm>
#include <cstring>

namespace goldrop {
class OtaImageWriter {
  public:
    virtual ~OtaImageWriter() = default;
    virtual bool write(const uint8_t *bytes, std::size_t size) = 0;
};

// Holds only the prefix until board/version validation succeeds. The caller hashes
// each complete received chunk exactly once, independently of prefix buffering.
class OtaImageStream {
  public:
    explicit OtaImageStream(const OtaManifest &manifest) : manifest_(manifest) {}
    bool accept(const uint8_t *data, std::size_t count, OtaImageWriter &writer) {
        if (!data || !count || count > manifest_.size - total_) {
            error_ = "image_size";
            return false;
        }
        std::size_t offset = 0;
        if (!checked_) {
            offset = std::min(count, prefix_.size() - prefix_size_);
            std::memcpy(prefix_.data() + prefix_size_, data, offset);
            prefix_size_ += offset;
            if (prefix_size_ == prefix_.size()) {
                if (!validate_firmware_prefix(prefix_.data(), prefix_.size(), manifest_)) {
                    error_ = "image_hardware_or_version";
                    return false;
                }
                if (!writer.write(prefix_.data(), prefix_.size())) {
                    error_ = "ota_header_write";
                    return false;
                }
                checked_ = true;
            }
        }
        if (checked_ && count > offset && !writer.write(data + offset, count - offset)) {
            error_ = "ota_write";
            return false;
        }
        total_ += count;
        return true;
    }
    bool complete() const { return checked_ && total_ == manifest_.size; }
    uint32_t total() const { return total_; }
    const char *error() const { return error_; }

  private:
    const OtaManifest &manifest_;
    std::array<uint8_t, kFirmwarePrefixSize> prefix_{};
    std::size_t prefix_size_{};
    uint32_t total_{};
    bool checked_{};
    const char *error_{"none"};
};
} // namespace goldrop
