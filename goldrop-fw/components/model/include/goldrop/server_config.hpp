#pragma once
#include "goldrop/settings.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace goldrop {
struct ServerConfig {
    std::array<char, settings::kMaxServerHostLength + 1> host{};
    uint16_t port{};
    static bool parse_ipv4(std::string_view text, std::array<uint8_t, 4>& result) {
        std::array<uint8_t, 4> parsed{};
        std::size_t offset = 0;
        for (unsigned part = 0; part < 4; ++part) {
            unsigned value = 0, digits = 0;
            const auto start = offset;
            while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9') {
                value = value * 10 + static_cast<unsigned>(text[offset++] - '0');
                if (++digits > 3 || value > 255) return false;
            }
            if (!digits || (digits > 1 && text[start] == '0')) return false;
            parsed[part] = static_cast<uint8_t>(value);
            if (part < 3 && (offset == text.size() || text[offset++] != '.')) return false;
        }
        if (offset != text.size() || parsed[0] == 0 || parsed[0] >= 224) return false;
        result = parsed;
        return true;
    }
    static bool valid_host(std::string_view text) {
        if (text.empty() || text.size() > settings::kMaxServerHostLength) return false;
        bool numeric = true;
        for (char c : text) if ((c < '0' || c > '9') && c != '.') numeric = false;
        if (numeric) { std::array<uint8_t, 4> address{}; return parse_ipv4(text, address); }
        std::size_t label_size = 0;
        char previous = 0;
        for (char c : text) {
            if (c == '.') {
                if (!label_size || previous == '-') return false;
                label_size = 0;
            } else {
                const bool alphanumeric = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                                          (c >= '0' && c <= '9');
                if ((!alphanumeric && c != '-') || (!label_size && c == '-') || ++label_size > 63) return false;
            }
            previous = c;
        }
        return label_size > 0 && previous != '-';
    }
    bool set_address(const char* text, std::size_t size) {
        if (!text || !valid_host(std::string_view(text, size))) return false;
        host.fill(0);
        for (std::size_t i = 0; i < size; ++i) host[i] = text[i] >= 'A' && text[i] <= 'Z' ? text[i] + ('a' - 'A') : text[i];
        return true;
    }
    bool set_port(const char* text, std::size_t size) {
        if (!text || size == 0 || size > 5) return false;
        uint32_t value = 0;
        for (std::size_t i = 0; i < size; ++i) {
            if (text[i] < '0' || text[i] > '9') return false;
            value = value * 10 + static_cast<unsigned>(text[i] - '0');
        }
        if (value == 0 || value > 65535) return false;
        port = static_cast<uint16_t>(value);
        return true;
    }
    bool valid() const {
        const auto* end = static_cast<const char*>(std::memchr(host.data(), 0, host.size()));
        return port && end && valid_host(std::string_view(host.data(), end - host.data()));
    }
};
}  // namespace goldrop
