#include "goldrop/backend_ack.hpp"
#include <charconv>
namespace goldrop {
namespace {
bool decimal(std::string_view text, uint64_t &value) {
    if (text.empty() || text.size() > 20 || (text.size() > 1 && text[0] == '0'))
        return false;
    for (char c : text)
        if (c < '0' || c > '9')
            return false;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
int hex(char c) {
    return c >= '0' && c <= '9'   ? c - '0'
           : c >= 'a' && c <= 'f' ? c - 'a' + 10
           : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                  : -1;
}
} // namespace
bool parse_backend_ack(std::string_view text, Measurement &out) {
    out = {};
    if (text.substr(0, 5) != "ACK1 " || text.size() < 26 || text[22] != ' ')
        return false;
    Measurement parsed;
    for (unsigned i = 0; i < 6; ++i) {
        const auto offset = 5 + 3 * i;
        const int a = hex(text[offset]), b = hex(text[offset + 1]);
        if (a < 0 || b < 0 || (i < 5 && text[offset + 2] != ':'))
            return false;
        parsed.device_id.mac[i] = uint8_t(a * 16 + b);
    }
    text.remove_prefix(23);
    const auto separator = text.find(' ');
    if (separator == text.npos || !parsed.device_id.valid() ||
        !decimal(text.substr(0, separator), parsed.boot_id) ||
        !decimal(text.substr(separator + 1), parsed.sequence) || !parsed.sequence)
        return false;
    out = parsed;
    return true;
}
void BackendAckReader::poll(int64_t now) {
    if (size_ && now - started_ >= 5000) {
        reset();
        discarding_ = true;
    }
}
bool BackendAckReader::feed(char byte, int64_t now, Measurement &out) {
    poll(now);
    if (byte == '\n') {
        bool valid = false;
        if (!discarding_) {
            auto text = std::string_view(bytes_.data(), size_);
            if (!text.empty() && text.back() == '\r')
                text.remove_suffix(1);
            valid = parse_backend_ack(text, out);
        }
        reset();
        return valid;
    }
    if (discarding_)
        return false;
    if (byte == '\0' || size_ == bytes_.size()) {
        reset();
        discarding_ = true;
        return false;
    }
    if (!size_)
        started_ = now;
    bytes_[size_++] = byte;
    return false;
}
} // namespace goldrop
