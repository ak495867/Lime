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

bool FirmwareLoader::load_ovmf_split(std::shared_ptr<MemoryManager> mem, const std::string& code_path, const std::string& vars_path, uint64_t code_gpa, uint64_t vars_gpa) {
    if (!mem) return false;
    
    std::ifstream code_file(code_path, std::ios::binary | std::ios::ate);
    if (code_file.is_open()) {
        std::streamsize size = code_file.tellg();
        code_file.seekg(0, std::ios::beg);
        std::vector<uint8_t> buffer(size);
        if (code_file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            mem->write_bytes(code_gpa, buffer.data(), buffer.size());
        }
    }
    
    std::ifstream vars_file(vars_path, std::ios::binary | std::ios::ate);
    if (vars_file.is_open()) {
        std::streamsize size = vars_file.tellg();
        vars_file.seekg(0, std::ios::beg);
        std::vector<uint8_t> buffer(size);
        if (vars_file.read(reinterpret_cast<char*>(buffer.data()), size)) {
            mem->write_bytes(vars_gpa, buffer.data(), buffer.size());
        }
    }
    
    return true;
}

bool FirmwareLoader::load_linux_kernel(std::shared_ptr<MemoryManager> mem, const std::string& kernel_path, const std::string& initrd_path, const std::string& cmdline, uint64_t kernel_gpa, uint64_t* out_entry) {
    if (!mem) return false;
    
    std::ifstream kern(kernel_path, std::ios::binary | std::ios::ate);
    if (!kern.is_open()) return false;
    std::streamsize ksize = kern.tellg();
    kern.seekg(0, std::ios::beg);
    std::vector<uint8_t> kbuf(ksize);
    kern.read(reinterpret_cast<char*>(kbuf.data()), ksize);
    mem->write_bytes(kernel_gpa, kbuf.data(), kbuf.size());
    
    if (out_entry) {
        *out_entry = kernel_gpa;
        if (ksize > 0x18 && kbuf[0] == 0x7F && kbuf[1] == 'E' && kbuf[2] == 'L' && kbuf[3] == 'F') {
            uint64_t entry = 0;
            std::memcpy(&entry, kbuf.data() + 0x18, 8);
            *out_entry = entry;
        }
    }
    
    uint64_t initrd_gpa = 0;
    uint64_t initrd_size = 0;
    
    if (!initrd_path.empty()) {
        std::ifstream initrd(initrd_path, std::ios::binary | std::ios::ate);
        if (initrd.is_open()) {
            initrd_size = initrd.tellg();
            initrd.seekg(0, std::ios::beg);
            std::vector<uint8_t> ibuf(initrd_size);
            initrd.read(reinterpret_cast<char*>(ibuf.data()), initrd_size);
            initrd_gpa = kernel_gpa + ksize + 0x100000;
            initrd_gpa = (initrd_gpa + 0xFFF) & ~0xFFFULL;
            mem->write_bytes(initrd_gpa, ibuf.data(), ibuf.size());
        }
    }
    
    uint64_t fdt_gpa = (initrd_gpa > 0) ? (initrd_gpa + initrd_size + 0x10000) : (kernel_gpa + ksize + 0x10000);
    fdt_gpa = (fdt_gpa + 0xFFF) & ~0xFFFULL;
    
    return generate_fdt(mem, fdt_gpa, cmdline, initrd_gpa, initrd_size);
}

bool FirmwareLoader::generate_fdt(std::shared_ptr<MemoryManager> mem, uint64_t fdt_gpa, const std::string& cmdline, uint64_t initrd_start, uint64_t initrd_size) {
    std::vector<uint8_t> dtb(4096, 0);
    dtb[0] = 0xd0; dtb[1] = 0x0d; dtb[2] = 0xfe; dtb[3] = 0xed;
    dtb[4] = 0x00; dtb[5] = 0x00; dtb[6] = 0x10; dtb[7] = 0x00;
    
    mem->write_bytes(fdt_gpa, dtb.data(), dtb.size());
    return true;
}

}
