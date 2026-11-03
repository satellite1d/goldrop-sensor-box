#pragma once
#include <cstdint>
#ifndef GOLDROP_FIRMWARE_VERSION
#define GOLDROP_FIRMWARE_VERSION "0.6.1"
#endif
#ifndef GOLDROP_HARDWARE_REVISION
#define GOLDROP_HARDWARE_REVISION "goldrop-c3-v1.0"
#endif
namespace goldrop {
inline constexpr char kFirmwareVersion[] = GOLDROP_FIRMWARE_VERSION;
inline constexpr char kHardwareRevision[] = GOLDROP_HARDWARE_REVISION;
struct FirmwareDescriptor {
    char magic[8];
    uint32_t format;
    char hardware[32];
    char version[32];
    uint32_t reserved;
};
static_assert(sizeof(FirmwareDescriptor) == 80);
static_assert(sizeof(kHardwareRevision) <= 32 && sizeof(kFirmwareVersion) <= 32);
inline constexpr unsigned kFirmwareDescriptorOffset = 24 + 8 + 256;
inline constexpr unsigned kFirmwarePrefixSize = kFirmwareDescriptorOffset + sizeof(FirmwareDescriptor);
} // namespace goldrop
