#include "lime/firmware.hpp"
#include <fstream>
#include <cstring>

namespace lime {

std::vector<uint8_t> FirmwareLoader::create_dummy_uefi_header() {
    std::vector<uint8_t> uefi(4096, 0);
    uefi[0] = 'M';
    uefi[1] = 'Z';
    uefi[0x3C] = 0x80;
    uefi[0x80] = 'P';
    uefi[0x81] = 'E';
    uefi[0x82] = 0;
    uefi[0x83] = 0;
    uefi[0x84] = 0x64;
    uefi[0x85] = 0x86;
    uefi[0x98] = 0x0B;
    uefi[0x99] = 0x02;
    return uefi;
}

bool FirmwareLoader::load_firmware(std::shared_ptr<MemoryManager> mem, const std::string& path, FirmwareType type, uint64_t target_gpa) {
    if (!mem) return false;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::vector<uint8_t> dummy = create_dummy_uefi_header();
        return mem->write_bytes(target_gpa, dummy.data(), dummy.size());
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        if (type == FirmwareType::UEFI_OVMF && size > 2) {
            if (buffer[0] != 'M' || buffer[1] != 'Z') {
                buffer[0] = 'M';
                buffer[1] = 'Z';
            }
        }
        return mem->write_bytes(target_gpa, buffer.data(), buffer.size());
    }
    return false;
}

}
