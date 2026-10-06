#include "lime/smbios.hpp"
#include <cstring>

namespace lime {

uint8_t SMBIOSTables::compute_checksum(const uint8_t* data, size_t length) {
    uint8_t checksum = 0;
    for (size_t i = 0; i < length; ++i) {
        checksum += data[i];
    }
    return (~checksum) + 1;
}

std::vector<uint8_t> SMBIOSTables::create_bios_info() {

    SMBIOSHeader header = {0, 0x1F, 0x0000};
    std::vector<uint8_t> bios_info(sizeof(SMBIOSHeader) + 0x1D);
    std::memcpy(bios_info.data(), &header, sizeof(SMBIOSHeader));

    std::string vendor = "LimeVM";
    bios_info[0x08] = 0x01;  

    std::memcpy(&bios_info[sizeof(SMBIOSHeader) + 0x00], vendor.c_str(), vendor.size() + 1);

    std::string version = "1.0.0";
    bios_info[0x09] = 0x01 + vendor.size() + 1;
    std::memcpy(&bios_info[sizeof(SMBIOSHeader) + 0x01 + vendor.size() + 1], version.c_str(), version.size() + 1);

    std::string date = "10/03/2026";
    bios_info[0x0A] = 0x01 + vendor.size() + 1 + version.size() + 1;
    std::memcpy(&bios_info[sizeof(SMBIOSHeader) + 0x01 + vendor.size() + 1 + version.size() + 1], date.c_str(), date.size() + 1);

    bios_info[0x0F] = 0x00;  

    bios_info[0x10] = 0x00;  
    bios_info[0x11] = 0x00;  
    bios_info[0x12] = 0x00;  
    bios_info[0x13] = 0x00;  
    bios_info[0x14] = 0x00;
    bios_info[0x15] = 0x00;
    bios_info[0x16] = 0x00;
    bios_info[0x17] = 0x00;

    bios_info[0x18] = 1;  
    bios_info[0x19] = 0;  

    bios_info[0x1A] = 0xFF;  
    bios_info[0x1B] = 0xFF;  
    
    return bios_info;
}

std::vector<uint8_t> SMBIOSTables::create_system_info() {

    SMBIOSHeader header = {1, 0x19, 0x0000};
    std::vector<uint8_t> system_info(sizeof(SMBIOSHeader) + 0x17);
    std::memcpy(system_info.data(), &header, sizeof(SMBIOSHeader));

    std::string manufacturer = "LimeVM";
    system_info[0x08] = 0x01;
    std::memcpy(&system_info[sizeof(SMBIOSHeader) + 0x00], manufacturer.c_str(), manufacturer.size() + 1);

    std::string product = "LimeVM Virtual Machine";
    system_info[0x09] = 0x01 + manufacturer.size() + 1;
    std::memcpy(&system_info[sizeof(SMBIOSHeader) + 0x01 + manufacturer.size() + 1], product.c_str(), product.size() + 1);

    std::string version = "1.0.0";
    system_info[0x0A] = 0x01 + manufacturer.size() + 1 + product.size() + 1;
    std::memcpy(&system_info[sizeof(SMBIOSHeader) + 0x01 + manufacturer.size() + 1 + product.size() + 1], version.c_str(), version.size() + 1);

    std::string serial = "LIMEVM000000001";
    system_info[0x0B] = 0x01 + manufacturer.size() + 1 + product.size() + 1 + version.size() + 1;
    std::memcpy(&system_info[sizeof(SMBIOSHeader) + 0x01 + manufacturer.size() + 1 + product.size() + 1 + version.size() + 1], serial.c_str(), serial.size() + 1);

    uint8_t uuid[16] = {
        0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF,
        0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10
    };
    std::memcpy(&system_info[sizeof(SMBIOSHeader) + 0x0C], uuid, 16);

    system_info[0x1C] = 0x06;  

    system_info[0x1D] = 0x01;
    std::string sku = "LIMEVM-STD";
    std::memcpy(&system_info[sizeof(SMBIOSHeader) + 0x01 + manufacturer.size() + 1 + product.size() + 1 + version.size() + 1 + serial.size() + 1 + 16], sku.c_str(), sku.size() + 1);

    system_info[0x1E] = 0x01;
    std::string family = "Virtual Machine";
    std::memcpy(&system_info[sizeof(SMBIOSHeader) + 0x01 + manufacturer.size() + 1 + product.size() + 1 + version.size() + 1 + serial.size() + 1 + 16 + sku.size() + 1], family.c_str(), family.size() + 1);
    
    return system_info;
}

std::vector<uint8_t> SMBIOSTables::create_processor_info(uint32_t cpu_count) {

    SMBIOSHeader header = {4, 0x2A, 0x0000};
    std::vector<uint8_t> processor_info(sizeof(SMBIOSHeader) + 0x28);
    std::memcpy(processor_info.data(), &header, sizeof(SMBIOSHeader));

    processor_info[0x08] = 0x01;
    std::string socket = "CPU Socket 1";
    std::memcpy(&processor_info[sizeof(SMBIOSHeader) + 0x00], socket.c_str(), socket.size() + 1);

    processor_info[0x09] = 0x03;

    processor_info[0x0A] = 0xB9;

    processor_info[0x0B] = 0x01;
    std::string manufacturer = "GenuineIntel";
    std::memcpy(&processor_info[sizeof(SMBIOSHeader) + 0x00], manufacturer.c_str(), manufacturer.size() + 1);

    processor_info[0x0C] = 0x00;
    processor_info[0x0D] = 0x00;
    processor_info[0x0E] = 0x00;
    processor_info[0x0F] = 0x00;

    processor_info[0x10] = 0x01;
    std::string version = "Intel(R) Core(TM) i7 CPU @ 3.00GHz";
    std::memcpy(&processor_info[sizeof(SMBIOSHeader) + 0x00], version.c_str(), version.size() + 1);

    processor_info[0x14] = 0x64;
    processor_info[0x15] = 0x00;

    processor_info[0x16] = 0xB8;
    processor_info[0x17] = 0x0B;

    processor_info[0x18] = 0xB8;
    processor_info[0x19] = 0x0B;

    processor_info[0x1A] = 0x60;  

    processor_info[0x1B] = 0x00;

    processor_info[0x1C] = 0x00;
    processor_info[0x1D] = 0x00;

    processor_info[0x1E] = 0x00;
    processor_info[0x1F] = 0x00;

    processor_info[0x20] = 0x00;
    processor_info[0x21] = 0x00;

    processor_info[0x22] = 0x01;
    std::string serial = "000000000000";
    std::memcpy(&processor_info[sizeof(SMBIOSHeader) + 0x00], serial.c_str(), serial.size() + 1);

    processor_info[0x23] = 0x01;
    std::string asset = "To Be Filled By O.E.M.";
    std::memcpy(&processor_info[sizeof(SMBIOSHeader) + 0x00], asset.c_str(), asset.size() + 1);

    processor_info[0x24] = 0x01;
    std::string part = "To Be Filled By O.E.M.";
    std::memcpy(&processor_info[sizeof(SMBIOSHeader) + 0x00], part.c_str(), part.size() + 1);

    processor_info[0x28] = 1;

    processor_info[0x29] = 1;

    processor_info[0x2A] = 1;

    processor_info[0x2C] = 0x00;
    processor_info[0x2D] = 0x00;

    processor_info[0x2E] = 0x00;
    processor_info[0x2F] = 0x00;

    processor_info[0x30] = 0x00;
    processor_info[0x31] = 0x00;

    processor_info[0x32] = 0x00;
    processor_info[0x33] = 0x00;

    processor_info[0x34] = 0x00;
    processor_info[0x35] = 0x00;
    
    return processor_info;
}

bool SMBIOSTables::generate_tables(std::shared_ptr<MemoryManager> mem, uint64_t base_gpa) {
    if (!mem) return false;
    
    uint64_t offset = base_gpa;

    auto bios_info = create_bios_info();
    if (!mem->write_bytes(offset, bios_info.data(), bios_info.size())) {
        return false;
    }
    offset += bios_info.size();

    auto system_info = create_system_info();
    if (!mem->write_bytes(offset, system_info.data(), system_info.size())) {
        return false;
    }
    offset += system_info.size();

    for (uint32_t i = 0; i < 1; ++i) {  
        auto processor_info = create_processor_info(1);
        if (!mem->write_bytes(offset, processor_info.data(), processor_info.size())) {
            return false;
        }
        offset += processor_info.size();
    }

    SMBIOSHeader end_header = {127, 0x04, 0x0000};
    std::vector<uint8_t> end_marker(sizeof(SMBIOSHeader));
    std::memcpy(end_marker.data(), &end_header, sizeof(SMBIOSHeader));
    
    if (!mem->write_bytes(offset, end_marker.data(), end_marker.size())) {
        return false;
    }
    
    return true;
}

}
