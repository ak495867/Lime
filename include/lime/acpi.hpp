#ifndef LIME_ACPI_HPP
#define LIME_ACPI_HPP

#include <cstdint>
#include <vector>
#include <memory>
#include "lime/memory.hpp"

namespace lime {

#pragma pack(push, 1)
struct RSDPDescriptor {
    char signature[8]{'R', 'S', 'D', ' ', 'P', 'T', 'R', ' '};
    uint8_t checksum{0};
    char oem_id[6]{'L', 'I', 'M', 'E', 'V', 'M'};
    uint8_t revision{2};
    uint32_t rsdt_address{0};
    uint32_t length{36};
    uint64_t xsdt_address{0};
    uint8_t extended_checksum{0};
    uint8_t reserved[3]{0, 0, 0};
};

struct ACPITableHeader {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
};
#pragma pack(pop)

class ACPITableBuilder {
public:
    static bool generate_tables(std::shared_ptr<MemoryManager> mem, uint64_t base_gpa = 0x000F0000, uint32_t cpu_count = 1);
    static uint8_t compute_checksum(const uint8_t* data, size_t length);
};

}

#endif
