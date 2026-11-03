#pragma once
#include "goldrop/managed_config.hpp"
namespace goldrop {
// GRC1 | random 96-bit nonce | AES-256-GCM ciphertext | 128-bit tag.
bool management_encrypt(const std::array<uint8_t, 32> &, DeviceId, char direction, const uint8_t *,
                        std::size_t, uint8_t *, std::size_t, std::size_t &);
bool management_decrypt(const std::array<uint8_t, 32> &, DeviceId, char direction, const uint8_t *,
                        std::size_t, uint8_t *, std::size_t, std::size_t &);
bool management_key_id(const std::array<uint8_t, 32> &, char (&hex)[17]);
} // namespace goldrop
