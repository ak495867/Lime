#ifndef LIME_FIRMWARE_HPP
#define LIME_FIRMWARE_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "lime/memory.hpp"

namespace lime {

enum class FirmwareType {
    UEFI_OVMF,
    SEABIOS_LEGACY,
    CUSTOM_DTB
};

class FirmwareLoader {
public:
    static bool load_firmware(std::shared_ptr<MemoryManager> mem, const std::string& path, FirmwareType type, uint64_t target_gpa = 0xFFF00000);
    static std::vector<uint8_t> create_dummy_uefi_header();
};

}

#endif
