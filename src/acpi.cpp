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

    RSDPDescriptor rsdp{};
    std::memcpy(rsdp.signature, "RSD PTR ", 8);
    rsdp.revision = 0;
    std::memcpy(rsdp.oem_id, "LIMEVM", 6);
    rsdp.rsdt_address = static_cast<uint32_t>(base_gpa + sizeof(RSDPDescriptor));
    rsdp.length = sizeof(RSDPDescriptor);
    rsdp.xsdt_address = 0;
    rsdp.checksum = compute_checksum(reinterpret_cast<const uint8_t*>(&rsdp), 20);
    rsdp.extended_checksum = compute_checksum(reinterpret_cast<const uint8_t*>(&rsdp), sizeof(rsdp));

    mem->write_bytes(base_gpa, &rsdp, sizeof(rsdp));

    struct RSDT {
        ACPITableHeader header;
        uint32_t entry[1];
    } rsdt{};
    
    std::memcpy(rsdt.header.signature, "RSDT", 4);
    rsdt.header.length = sizeof(RSDT);
    rsdt.header.revision = 1;
    std::memcpy(rsdt.header.oem_id, "LIMEVM", 6);
    std::memcpy(rsdt.header.oem_table_id, "LIMERSDT", 8);
    rsdt.header.oem_revision = 1;
    rsdt.header.creator_id = 0x454D494C;
    rsdt.header.creator_revision = 1;
    rsdt.entry[0] = static_cast<uint32_t>(base_gpa + sizeof(RSDPDescriptor) + sizeof(RSDT));
    rsdt.header.checksum = compute_checksum(reinterpret_cast<const uint8_t*>(&rsdt), sizeof(rsdt));

    mem->write_bytes(base_gpa + sizeof(RSDPDescriptor), &rsdt, sizeof(rsdt));

    struct MADT {
        ACPITableHeader header;
        uint32_t local_apic_address;
        uint32_t flags;
    } madt{};
    
    std::memcpy(madt.header.signature, "APIC", 4);
    madt.header.length = sizeof(MADT) + (cpu_count * 8);
    madt.header.revision = 1;
    std::memcpy(madt.header.oem_id, "LIMEVM", 6);
    std::memcpy(madt.header.oem_table_id, "LIMEMADT", 8);
    madt.header.oem_revision = 1;
    madt.header.creator_id = 0x454D494C;
    madt.header.creator_revision = 1;
    madt.local_apic_address = 0xFEE00000;
    madt.flags = 1;

    std::vector<uint8_t> madt_full(madt.header.length, 0);
    std::memcpy(madt_full.data(), &madt, sizeof(MADT));
    
    for (uint32_t i = 0; i < cpu_count; ++i) {
        uint8_t* entry = madt_full.data() + sizeof(MADT) + (i * 8);
        entry[0] = 0;
        entry[1] = 8;
        entry[2] = static_cast<uint8_t>(i);
        entry[3] = static_cast<uint8_t>(i);
        entry[4] = 1;
        entry[5] = 0;
        entry[6] = 0;
        entry[7] = 0;
    }
    
    ACPITableHeader* madt_hdr = reinterpret_cast<ACPITableHeader*>(madt_full.data());
    madt_hdr->checksum = compute_checksum(madt_full.data(), madt_full.size());

    mem->write_bytes(base_gpa + sizeof(RSDPDescriptor) + sizeof(RSDT), madt_full.data(), madt_full.size());
    return true;
}

}
