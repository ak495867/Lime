#ifndef LIME_X86_CPU_HPP
#define LIME_X86_CPU_HPP

#include <cstdint>
#include <array>
#include <memory>
#include "lime/memory.hpp"
#include "lime/devices.hpp"

namespace lime {

enum class CPUMode {
    REAL_16BIT,
    PROTECTED_32BIT,
    LONG_64BIT
};

enum class TargetArch {
    RISCV64,
    X86_64,
    ARM64
};

class X86CPUDecoder {
public:
    X86CPUDecoder(std::shared_ptr<MemoryManager> mem, std::shared_ptr<DeviceBus> bus);
    ~X86CPUDecoder() = default;

    void reset(uint64_t entry_point);
    bool step();

    uint64_t get_gpr(size_t index) const;
    void set_gpr(size_t index, uint64_t val);

    uint64_t get_rip() const;
    void set_rip(uint64_t rip);

    CPUMode mode() const;
    void set_mode(CPUMode m);

private:
    std::shared_ptr<MemoryManager> mem_;
    std::shared_ptr<DeviceBus> bus_;

    std::array<uint64_t, 16> gprs_{};
    uint64_t rip_{0};
    uint64_t rflags_{0x02};
    uint64_t cr0_{0x60000010};
    uint64_t cr3_{0};
    uint64_t cr4_{0};
    CPUMode mode_{CPUMode::REAL_16BIT};
};

}

#endif
