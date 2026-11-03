#pragma once
#include "goldrop/managed_config.hpp"
namespace goldrop {
using ManagementChallenge = std::array<uint8_t, 16>;
using ManagementRequest = std::array<uint8_t, 136>;
using ManagementResponse = std::array<uint8_t, 512>;
void encode_management_request(const ManagedConfigState &, const ManagementChallenge &, bool wifi, bool probe,
                               uint64_t exchange_sequence, ManagementRequest &);
bool decode_management_response(const ManagementResponse &, const ManagementChallenge &, ConfigCommand &);
} // namespace goldrop
