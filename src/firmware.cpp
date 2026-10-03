#include "lime/firmware.hpp"
#include <fstream>

namespace lime {

std::vector<uint8_t> FirmwareLoader::create_dummy_uefi_header() {
    std::vector<uint8_t> uefi(4096, 0);
    uefi[0] = 'M';
    uefi[1] = 'Z';
    uefi[0x3C] = 0x80;
    uefi[0x80] = 'P';
    uefi[0x81] = 'E';
    return uefi;
}

bool FirmwareLoader::load_firmware(std::shared_ptr<MemoryManager> mem, const std::string& path, FirmwareType, uint64_t target_gpa) {
    if (!mem) return false;

    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        std::vector<uint8_t> dummy = create_dummy_uefi_header();
        return mem->write_bytes(target_gpa, dummy.data(), dummy.size());
    }

    std::vector<uint8_t> buffer((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    return mem->write_bytes(target_gpa, buffer.data(), buffer.size());
}

}
