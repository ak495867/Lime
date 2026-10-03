#include "lime/acpi.hpp"
#include <cstring>

namespace lime {

uint8_t ACPITableBuilder::compute_checksum(const uint8_t* data, size_t length) {
    uint8_t sum = 0;
    for (size_t i = 0; i < length; ++i) {
        sum += data[i];
    }
    return static_cast<uint8_t>(0 - sum);
}

bool ACPITableBuilder::generate_tables(std::shared_ptr<MemoryManager> mem, uint64_t base_gpa, uint32_t cpu_count) {
    if (!mem) return false;

    RSDPDescriptor rsdp;
    rsdp.rsdt_address = static_cast<uint32_t>(base_gpa + sizeof(RSDPDescriptor));
    rsdp.xsdt_address = base_gpa + sizeof(RSDPDescriptor) + 64;
    rsdp.checksum = compute_checksum(reinterpret_cast<const uint8_t*>(&rsdp), 20);
    rsdp.extended_checksum = compute_checksum(reinterpret_cast<const uint8_t*>(&rsdp), sizeof(rsdp));

    mem->write_bytes(base_gpa, &rsdp, sizeof(rsdp));

    ACPITableHeader madt;
    std::memcpy(madt.signature, "APIC", 4);
    madt.length = sizeof(ACPITableHeader) + (cpu_count * 8);
    madt.revision = 1;
    std::memcpy(madt.oem_id, "LIMEVM", 6);
    std::memcpy(madt.oem_table_id, "LIMEMADT", 8);
    madt.oem_revision = 1;
    madt.creator_id = 0x454D494C;
    madt.creator_revision = 1;
    madt.checksum = compute_checksum(reinterpret_cast<const uint8_t*>(&madt), sizeof(madt));

    mem->write_bytes(base_gpa + sizeof(RSDPDescriptor), &madt, sizeof(madt));
    return true;
}

}
