#ifndef LIME_SMBIOS_HPP
#define LIME_SMBIOS_HPP

#include <cstdint>
#include <vector>
#include <memory>
#include "lime/memory.hpp"

namespace lime {

struct SMBIOSHeader {
    uint8_t type;
    uint8_t length;
    uint16_t handle;
};

class SMBIOSTables {
public:
    static bool generate_tables(std::shared_ptr<MemoryManager> mem, uint64_t base_gpa = 0x000F8000);
    static std::vector<uint8_t> create_bios_info();
    static std::vector<uint8_t> create_system_info();
    static std::vector<uint8_t> create_processor_info(uint32_t cpu_count);

private:
    static uint8_t compute_checksum(const uint8_t* data, size_t length);
};

}

#endif
