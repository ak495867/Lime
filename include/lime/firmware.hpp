#ifndef LIME_FIRMWARE_HPP
#define LIME_FIRMWARE_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include "lime/memory.hpp"

namespace lime {

enum class FirmwareType {
    UEFI_OVMF,
    SEABIOS_LEGACY,
    CUSTOM_DTB
};

class FirmwareLoader {
public:
    static bool load_firmware(std::shared_ptr<MemoryManager> mem, const std::string& path, FirmwareType type, uint64_t target_gpa = 0xFFF00000);
    
    static bool load_ovmf_split(std::shared_ptr<MemoryManager> mem, const std::string& code_path, const std::string& vars_path, uint64_t code_gpa = 0xFFC00000, uint64_t vars_gpa = 0xFFB00000);
    
    static bool load_linux_kernel(std::shared_ptr<MemoryManager> mem, const std::string& kernel_path, const std::string& initrd_path, const std::string& cmdline, uint64_t kernel_gpa = 0x80200000, uint64_t* out_entry = nullptr);
    
    static std::vector<uint8_t> create_dummy_uefi_header();
    
private:
    static bool generate_fdt(std::shared_ptr<MemoryManager> mem, uint64_t fdt_gpa, const std::string& cmdline, uint64_t initrd_start, uint64_t initrd_size);
};

}

#endif
