#include "goldrop/firmware_identity.hpp"
extern "C" {
extern const goldrop::FirmwareDescriptor goldrop_firmware_descriptor
    __attribute__((section(".rodata_custom_desc"), used)) = {
        {'G', 'D', 'R', 'O', 'P', 'F', 'W', '1'}, 1, GOLDROP_HARDWARE_REVISION, GOLDROP_FIRMWARE_VERSION, 0};
}
