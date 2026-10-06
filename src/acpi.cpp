#include "lime/acpi.hpp"
#include <cstring>
#include <vector>

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

    uint64_t rsdt_gpa = base_gpa + sizeof(RSDPDescriptor);
    uint64_t fadt_gpa = rsdt_gpa + 64;
    uint64_t dsdt_gpa = fadt_gpa + 256;
    uint64_t madt_gpa = dsdt_gpa + 4096;

    RSDPDescriptor rsdp{};
    std::memcpy(rsdp.signature, "RSD PTR ", 8);
    rsdp.revision = 0;
    std::memcpy(rsdp.oem_id, "LIMEVM", 6);
    rsdp.rsdt_address = static_cast<uint32_t>(rsdt_gpa);
    rsdp.length = sizeof(RSDPDescriptor);
    rsdp.xsdt_address = 0;
    rsdp.checksum = compute_checksum(reinterpret_cast<const uint8_t*>(&rsdp), 20);
    rsdp.extended_checksum = compute_checksum(reinterpret_cast<const uint8_t*>(&rsdp), sizeof(rsdp));
    mem->write_bytes(base_gpa, &rsdp, sizeof(rsdp));

    struct RSDT {
        ACPITableHeader header;
        uint32_t entries[2];
    } rsdt{};
    
    std::memcpy(rsdt.header.signature, "RSDT", 4);
    rsdt.header.length = sizeof(RSDT);
    rsdt.header.revision = 1;
    std::memcpy(rsdt.header.oem_id, "LIMEVM", 6);
    std::memcpy(rsdt.header.oem_table_id, "LIMERSDT", 8);
    rsdt.header.oem_revision = 1;
    rsdt.header.creator_id = 0x454D494C;
    rsdt.header.creator_revision = 1;
    rsdt.entries[0] = static_cast<uint32_t>(fadt_gpa);
    rsdt.entries[1] = static_cast<uint32_t>(madt_gpa);
    rsdt.header.checksum = compute_checksum(reinterpret_cast<const uint8_t*>(&rsdt), sizeof(rsdt));
    mem->write_bytes(rsdt_gpa, &rsdt, sizeof(rsdt));

    std::vector<uint8_t> dsdt(36, 0);
    ACPITableHeader* dsdt_hdr = reinterpret_cast<ACPITableHeader*>(dsdt.data());
    std::memcpy(dsdt_hdr->signature, "DSDT", 4);
    dsdt_hdr->length = 36;
    dsdt_hdr->revision = 1;
    std::memcpy(dsdt_hdr->oem_id, "LIMEVM", 6);
    std::memcpy(dsdt_hdr->oem_table_id, "LIMEDSDT", 8);
    dsdt_hdr->oem_revision = 1;
    dsdt_hdr->creator_id = 0x454D494C;
    dsdt_hdr->creator_revision = 1;
    dsdt_hdr->checksum = compute_checksum(dsdt.data(), dsdt.size());
    mem->write_bytes(dsdt_gpa, dsdt.data(), dsdt.size());

    struct FADT {
        ACPITableHeader header;
        uint32_t firmware_ctrl;
        uint32_t dsdt;
        uint8_t  reserved;
        uint8_t  preferred_pm_profile;
        uint16_t sci_int;
        uint32_t smi_cmd;
        uint8_t  acpi_enable;
        uint8_t  acpi_disable;
        uint8_t  s4bios_req;
        uint8_t  pstate_cnt;
        uint32_t pm1a_evt_blk;
        uint32_t pm1b_evt_blk;
        uint32_t pm1a_cnt_blk;
        uint32_t pm1b_cnt_blk;
        uint32_t pm2_cnt_blk;
        uint32_t pm_tmr_blk;
        uint32_t gpe0_blk;
        uint32_t gpe1_blk;
        uint8_t  pm1_evt_len;
        uint8_t  pm1_cnt_len;
        uint8_t  pm2_cnt_len;
        uint8_t  pm_tmr_len;
        uint8_t  gpe0_blk_len;
        uint8_t  gpe1_blk_len;
        uint8_t  gpe1_base;
        uint8_t  cst_cnt;
        uint16_t p_lvl2_lat;
        uint16_t p_lvl3_lat;
        uint16_t flush_size;
        uint16_t flush_stride;
        uint8_t  duty_offset;
        uint8_t  duty_width;
        uint8_t  day_alrm;
        uint8_t  mon_alrm;
        uint8_t  century;
        uint16_t iapc_boot_arch;
        uint8_t  reserved2;
        uint32_t flags;
    } fadt{};

    std::memcpy(fadt.header.signature, "FACP", 4);
    fadt.header.length = sizeof(FADT);
    fadt.header.revision = 1;
    std::memcpy(fadt.header.oem_id, "LIMEVM", 6);
    std::memcpy(fadt.header.oem_table_id, "LIMEFACP", 8);
    fadt.header.oem_revision = 1;
    fadt.header.creator_id = 0x454D494C;
    fadt.header.creator_revision = 1;
    fadt.dsdt = static_cast<uint32_t>(dsdt_gpa);
    fadt.header.checksum = compute_checksum(reinterpret_cast<const uint8_t*>(&fadt), sizeof(fadt));
    mem->write_bytes(fadt_gpa, &fadt, sizeof(fadt));

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
    mem->write_bytes(madt_gpa, madt_full.data(), madt_full.size());

    return true;
}

}
