#pragma once
#include <cstddef>
#include <cstdint>
// Deterministic nonces exclusively for interoperability vectors on the host.
inline void esp_fill_random(void *buffer, std::size_t size) {
    auto *p = static_cast<uint8_t *>(buffer);
    for (std::size_t i = 0; i < size; ++i)
        p[i] = uint8_t(i);
}
