#pragma once
#include <cstddef>
#include <cstdint>
namespace goldrop::binary {
inline void put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        p[i] = uint8_t(v >> (8 * i));
}
inline void put64(uint8_t *p, uint64_t v) {
    for (unsigned i = 0; i < 8; ++i)
        p[i] = uint8_t(v >> (8 * i));
}
inline uint32_t get32(const uint8_t *p) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i)
        v |= uint32_t(p[i]) << (8 * i);
    return v;
}
inline uint64_t get64(const uint8_t *p) {
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i)
        v |= uint64_t(p[i]) << (8 * i);
    return v;
}
inline uint32_t crc32(const uint8_t *p, std::size_t n) {
    uint32_t v = 0xffffffff;
    for (std::size_t i = 0; i < n; ++i) {
        v ^= p[i];
        for (unsigned b = 0; b < 8; ++b)
            v = (v >> 1) ^ ((v & 1) ? 0xedb88320 : 0);
    }
    return ~v;
}
inline bool erased(const uint8_t *p, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i)
        if (p[i] != 0xff)
            return false;
    return true;
}
} // namespace goldrop::binary
