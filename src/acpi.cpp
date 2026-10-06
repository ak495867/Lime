#include "lime/acpi.hpp"
#include <cstring>

namespace lime {

uint8_t ACPITableBuilder::compute_checksum(const uint8_t* data, size_t length) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < length; ++i) {
        checksum += data[i];
    }
    return (~checksum) + 1;
}

bool ACPITableBuilder::generate_tables(std::shared_ptr<MemoryManager> mem, uint64_t base_gpa, uint32_t cpu_count) {
    if (!mem) return false;

    RSDPDescriptor rsdp;
    std::memcpy(rsdp.signature, "RSD PTR ", 8);
    rsdp.checksum = compute_checksum(reinterpret_cast<const uint8_t*>(&rsdp), sizeof(RSDPDescriptor));
    std::memcpy(rsdp.oem_id, "LIME", 6);
    rsdp.revision = 2;
    rsdp.length = 36;
    rsdp.rsdt_address = base_gpa + 0x100;

    if (!mem->write_bytes(base_gpa, reinterpret_cast<const uint8_t*>(&rsdp), sizeof(RSDPDescriptor))) {
        return false;
    }

    ACPITableHeader rsdt;
    std::memcpy(rsdt.signature, "RSDT", 4);
    rsdt.length = sizeof(ACPITableHeader) + (sizeof(uint32_t) * cpu_count);
    rsdt.revision = 1;
    rsdt.checksum = 0;  
    std::memcpy(rsdt.oem_id, "LIME", 6);
    std::memcpy(rsdt.oem_table_id, "RSDSC", 8);
    rsdt.oem_revision = 1;

    rsdt.checksum = compute_checksum(reinterpret_cast<const uint8_t*>(&rsdt), rsdt.length);

    if (!mem->write_bytes(base_gpa + 0x100, reinterpret_cast<const uint8_t*>(&rsdt), rsdt.length)) {
        return false;
    }

    return true;
}

}
