#include "lime/firmware.hpp"
#include <fstream>
#include <cstring>
#include <unordered_map>

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
    (void)type;

    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return false;

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    if (size <= 0) return false;
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    return file.read(reinterpret_cast<char*>(buffer.data()), size) &&
           mem->write_bytes(target_gpa, buffer.data(), buffer.size());
}

bool FirmwareLoader::load_ovmf_split(std::shared_ptr<MemoryManager> mem, const std::string& code_path, const std::string& vars_path, uint64_t code_gpa, uint64_t vars_gpa) {
    if (!mem) return false;
    
    std::ifstream code_file(code_path, std::ios::binary | std::ios::ate);
    if (code_file.is_open()) {
        std::streamsize size = code_file.tellg();
        code_file.seekg(0, std::ios::beg);
        std::vector<uint8_t> buffer(size);
        if (size <= 0 || !code_file.read(reinterpret_cast<char*>(buffer.data()), size) ||
            !mem->write_bytes(code_gpa, buffer.data(), buffer.size())) return false;
    } else return false;
    
    std::ifstream vars_file(vars_path, std::ios::binary | std::ios::ate);
    if (vars_file.is_open()) {
        std::streamsize size = vars_file.tellg();
        vars_file.seekg(0, std::ios::beg);
        std::vector<uint8_t> buffer(size);
        if (size <= 0 || !vars_file.read(reinterpret_cast<char*>(buffer.data()), size) ||
            !mem->write_bytes(vars_gpa, buffer.data(), buffer.size())) return false;
    } else return false;

    return true;
}

bool FirmwareLoader::load_linux_kernel(std::shared_ptr<MemoryManager> mem, const std::string& kernel_path, const std::string& initrd_path, const std::string& cmdline, uint64_t kernel_gpa, uint64_t* out_entry) {
    if (!mem) return false;
    
    std::ifstream kern(kernel_path, std::ios::binary | std::ios::ate);
    if (!kern.is_open()) return false;
    std::streamsize ksize = kern.tellg();
    kern.seekg(0, std::ios::beg);
    std::vector<uint8_t> kbuf(ksize);
    if (ksize <= 0 || !kern.read(reinterpret_cast<char*>(kbuf.data()), ksize) ||
        !mem->write_bytes(kernel_gpa, kbuf.data(), kbuf.size())) return false;
    
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
            initrd_gpa = kernel_gpa + ksize + 0x100000;
            initrd_gpa = (initrd_gpa + 0xFFF) & ~0xFFFULL;
            if (initrd_size == 0 || !initrd.read(reinterpret_cast<char*>(ibuf.data()), initrd_size) ||
                !mem->write_bytes(initrd_gpa, ibuf.data(), ibuf.size())) return false;
        } else return false;
    }
    
    uint64_t fdt_gpa = (initrd_gpa > 0) ? (initrd_gpa + initrd_size + 0x10000) : (kernel_gpa + ksize + 0x10000);
    fdt_gpa = (fdt_gpa + 0xFFF) & ~0xFFFULL;
    
    return generate_fdt(mem, fdt_gpa, cmdline, initrd_gpa, initrd_size);
}

class FdtBuilder {
public:
    void begin_node(const std::string& name) {
        write_u32(0x00000001); // FDT_BEGIN_NODE
        write_string_padded(name);
    }

    void end_node() {
        write_u32(0x00000002); // FDT_END_NODE
    }

    void prop_u32(const std::string& name, uint32_t val) {
        uint32_t be = to_be32(val);
        prop(name, &be, 4);
    }

    void prop_u64(const std::string& name, uint64_t val) {
        uint32_t be[2] = {to_be32(static_cast<uint32_t>(val >> 32)), to_be32(static_cast<uint32_t>(val))};
        prop(name, be, 8);
    }

    void prop_reg_2_2(const std::string& name, uint64_t addr, uint64_t size) {
        uint32_t be[4] = {
            to_be32(static_cast<uint32_t>(addr >> 32)),
            to_be32(static_cast<uint32_t>(addr)),
            to_be32(static_cast<uint32_t>(size >> 32)),
            to_be32(static_cast<uint32_t>(size))
        };
        prop(name, be, 16);
    }

    void prop_string(const std::string& name, const std::string& val) {
        prop(name, val.c_str(), val.size() + 1);
    }

    void prop_empty(const std::string& name) {
        prop(name, nullptr, 0);
    }

    void prop(const std::string& name, const void* data, size_t len) {
        uint32_t str_off = add_string(name);
        write_u32(0x00000003); // FDT_PROP
        write_u32(static_cast<uint32_t>(len));
        write_u32(str_off);
        if (len > 0 && data) {
            const uint8_t* p = reinterpret_cast<const uint8_t*>(data);
            struct_data_.insert(struct_data_.end(), p, p + len);
            while (struct_data_.size() % 4 != 0) {
                struct_data_.push_back(0);
            }
        }
    }

    std::vector<uint8_t> finish() {
        write_u32(0x00000009); // FDT_END

        uint32_t off_rsvmap = 40;
        uint32_t off_struct = off_rsvmap + 16;
        uint32_t off_strings = off_struct + static_cast<uint32_t>(struct_data_.size());
        uint32_t total_size = off_strings + static_cast<uint32_t>(string_data_.size());

        std::vector<uint8_t> out(total_size, 0);

        auto put32 = [&](size_t offset, uint32_t v) {
            uint32_t be = to_be32(v);
            std::memcpy(out.data() + offset, &be, 4);
        };

        // Header (40 bytes)
        put32(0, 0xd00dfeed);      // magic
        put32(4, total_size);      // totalsize
        put32(8, off_struct);      // off_dt_struct
        put32(12, off_strings);    // off_dt_strings
        put32(16, off_rsvmap);     // off_mem_rsvmap
        put32(20, 17);             // version
        put32(24, 16);             // last_comp_version
        put32(28, 0);              // boot_cpuid_phys
        put32(32, static_cast<uint32_t>(string_data_.size())); // size_dt_strings
        put32(36, static_cast<uint32_t>(struct_data_.size())); // size_dt_struct

        // Memory reservation block is zeroed (terminator) at off_rsvmap (40..56)

        // Structure block
        if (!struct_data_.empty()) {
            std::memcpy(out.data() + off_struct, struct_data_.data(), struct_data_.size());
        }

        // Strings block
        if (!string_data_.empty()) {
            std::memcpy(out.data() + off_strings, string_data_.data(), string_data_.size());
        }

        return out;
    }

private:
    static uint32_t to_be32(uint32_t x) {
        return ((x & 0xFF000000) >> 24) |
               ((x & 0x00FF0000) >> 8) |
               ((x & 0x0000FF00) << 8) |
               ((x & 0x000000FF) << 24);
    }

    void write_u32(uint32_t val) {
        uint32_t be = to_be32(val);
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&be);
        struct_data_.insert(struct_data_.end(), p, p + 4);
    }

    void write_string_padded(const std::string& str) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(str.c_str());
        struct_data_.insert(struct_data_.end(), p, p + str.size() + 1);
        while (struct_data_.size() % 4 != 0) {
            struct_data_.push_back(0);
        }
    }

    uint32_t add_string(const std::string& s) {
        auto it = str_offsets_.find(s);
        if (it != str_offsets_.end()) return it->second;
        uint32_t off = static_cast<uint32_t>(string_data_.size());
        const uint8_t* p = reinterpret_cast<const uint8_t*>(s.c_str());
        string_data_.insert(string_data_.end(), p, p + s.size() + 1);
        str_offsets_[s] = off;
        return off;
    }

    std::vector<uint8_t> struct_data_;
    std::vector<uint8_t> string_data_;
    std::unordered_map<std::string, uint32_t> str_offsets_;
};

std::vector<uint8_t> FirmwareLoader::generate_dtb(uint64_t ram_size, const std::string& cmdline, uint64_t initrd_start, uint64_t initrd_size) {
    if (ram_size == 0) ram_size = 128 * 1024 * 1024;

    FdtBuilder b;
    // Root node
    b.begin_node("");
    b.prop_u32("#address-cells", 2);
    b.prop_u32("#size-cells", 2);
    b.prop_string("compatible", "riscv-virtio");
    b.prop_string("model", "lime,riscv-virt");

    // /chosen
    b.begin_node("chosen");
    b.prop_string("bootargs", cmdline.empty() ? "console=hvc0 earlycon" : cmdline);
    b.prop_string("stdout-path", "/soc/uart@10000000");
    if (initrd_start > 0 && initrd_size > 0) {
        b.prop_u64("linux,initrd-start", initrd_start);
        b.prop_u64("linux,initrd-end", initrd_start + initrd_size);
    }
    b.end_node();

    // /cpus
    b.begin_node("cpus");
    b.prop_u32("#address-cells", 1);
    b.prop_u32("#size-cells", 0);
    b.prop_u32("timebase-frequency", 10000000);

    b.begin_node("cpu@0");
    b.prop_string("device_type", "cpu");
    b.prop_u32("reg", 0);
    b.prop_string("status", "okay");
    b.prop_string("compatible", "riscv");
    b.prop_string("riscv,isa", "rv64gc");
    b.prop_string("mmu-type", "riscv,sv39");

    b.begin_node("interrupt-controller");
    b.prop_u32("#interrupt-cells", 1);
    b.prop_empty("interrupt-controller");
    b.prop_string("compatible", "riscv,cpu-intc");
    b.end_node(); // interrupt-controller

    b.end_node(); // cpu@0
    b.end_node(); // cpus

    // /memory@80000000
    b.begin_node("memory@80000000");
    b.prop_string("device_type", "memory");
    b.prop_reg_2_2("reg", 0x80000000ULL, ram_size);
    b.end_node();

    // /soc
    b.begin_node("soc");
    b.prop_u32("#address-cells", 2);
    b.prop_u32("#size-cells", 2);
    b.prop_string("compatible", "simple-bus");
    b.prop_empty("ranges");

    // /soc/uart@10000000
    b.begin_node("uart@10000000");
    b.prop_string("compatible", "ns16550a");
    b.prop_reg_2_2("reg", 0x10000000ULL, 0x100ULL);
    b.prop_u32("clock-frequency", 3686400);
    b.prop_u32("interrupts", 10);
    b.end_node();

    // /soc/virtio@10001000
    b.begin_node("virtio@10001000");
    b.prop_string("compatible", "virtio,mmio");
    b.prop_reg_2_2("reg", 0x10001000ULL, 0x1000ULL);
    b.prop_u32("interrupts", 1);
    b.end_node();

    // /soc/clint@2000000
    b.begin_node("clint@2000000");
    b.prop_string("compatible", "riscv,clint0");
    b.prop_reg_2_2("reg", 0x02000000ULL, 0x10000ULL);
    b.end_node();

    // /soc/plic@c000000
    b.begin_node("plic@c000000");
    b.prop_string("compatible", "riscv,plic0");
    b.prop_reg_2_2("reg", 0x0c000000ULL, 0x4000000ULL);
    b.prop_u32("#interrupt-cells", 1);
    b.prop_empty("interrupt-controller");
    b.prop_u32("riscv,ndev", 32);
    b.end_node();

    b.end_node(); // soc

    b.end_node(); // root

    return b.finish();
}

bool FirmwareLoader::generate_fdt(std::shared_ptr<MemoryManager> mem, uint64_t fdt_gpa, const std::string& cmdline, uint64_t initrd_start, uint64_t initrd_size) {
    if (!mem) return false;
    auto dtb = generate_dtb(mem->total_capacity_bytes(), cmdline, initrd_start, initrd_size);
    return mem->write_bytes(fdt_gpa, dtb.data(), dtb.size());
}

}
