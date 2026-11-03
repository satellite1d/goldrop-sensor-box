#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace goldrop {
inline void clear_secret(void* memory, std::size_t size) {
    auto* bytes = static_cast<volatile uint8_t*>(memory);
    while (size--) *bytes++ = 0;
}

struct WifiCredentials {
    std::array<char, 33> ssid{};
    std::array<char, 64> password{};
    bool set_ssid(const char* value, std::size_t size) {
        if (!value || size == 0 || size > 32) return false;
        for (std::size_t i = 0; i < size; ++i) {
            if (static_cast<unsigned char>(value[i]) < 0x20 || value[i] == 0x7F) return false;
        }
        ssid.fill(0);
        std::memcpy(ssid.data(), value, size);
        return true;
    }
    bool set_password(const char* value, std::size_t size) {
        if (!value || size < 8 || size > 63) return false;
        for (std::size_t i = 0; i < size; ++i) {
            if (static_cast<unsigned char>(value[i]) < 0x20 ||
                static_cast<unsigned char>(value[i]) > 0x7E) return false;
        }
        clear_secret(password.data(), password.size());
        std::memcpy(password.data(), value, size);
        return true;
    }
    bool valid() const {
        const auto* s = static_cast<const char*>(std::memchr(ssid.data(), 0, ssid.size()));
        const auto* p = static_cast<const char*>(std::memchr(password.data(), 0, password.size()));
        if (!s || !p) return false;
        WifiCredentials check;
        const bool result = check.set_ssid(ssid.data(), s - ssid.data()) &&
                            check.set_password(password.data(), p - password.data());
        check.clear();
        return result;
    }
    void clear() { clear_secret(this, sizeof(*this)); }
};
}  // namespace goldrop
