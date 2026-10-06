#ifndef LIME_VCPU_HPP
#define LIME_VCPU_HPP

#include <cstdint>
#include <vector>
#include <array>
#include <memory>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <functional>
#include "lime/memory.hpp"
#include "lime/devices.hpp"
#include "lime/mmu.hpp"
#include "lime/interrupts.hpp"
#include "lime/jit.hpp"

namespace lime {

enum class VCPUState {
    RUNNING,
    IDLE_WAIT,
    HALTED,
    TRAPPED
};

enum class VirtMode {
    SOFTWARE_INTERPRETER,
    HARDWARE_VIRTUALIZATION
};

class VCPU {
public:
    VCPU(uint32_t id, std::shared_ptr<MemoryManager> mem, std::shared_ptr<DeviceBus> bus);
    ~VCPU() = default;

    void reset(uint64_t entry_point);
    bool step();
    size_t run_cycles(size_t max_cycles);
    void compile_block(BasicBlock& bb);
    bool execute_block_fast(const BasicBlock* bb, size_t& executed);

    uint64_t get_reg(size_t idx) const;
    void set_reg(size_t idx, uint64_t val);

    uint64_t get_pc() const;
    void set_pc(uint64_t pc);

    uint64_t get_csr(uint32_t csr_addr) const;
    void set_csr(uint32_t csr_addr, uint64_t val);

    VCPUState state() const;
    void set_state(VCPUState state);

    PrivilegeMode privilege_mode() const;
    void set_privilege_mode(PrivilegeMode mode);

    uint64_t total_cycles() const;
    uint32_t id() const;

    void trigger_interrupt(uint64_t cause);
    void set_virt_mode(VirtMode mode);
    VirtMode virt_mode() const;

    void attach_clint(std::shared_ptr<ClintDevice> clint);
    void attach_plic(std::shared_ptr<PlicDevice> plic);

    void execute_lr(uint32_t rd, uint32_t rs1, uint32_t funct3);
    void execute_sc(uint32_t rd, uint32_t rs1, uint32_t rs2, uint32_t funct3);
    bool lr_valid() const { return lr_valid_; }
    uint64_t lr_addr() const { return lr_addr_; }

    bool execute_compressed(uint16_t inst);

private:
    uint32_t fetch32(uint64_t addr, bool& fault);
    uint16_t fetch16(uint64_t addr, bool& fault);
    void execute_instruction(uint32_t inst);

    uint32_t id_;
    std::shared_ptr<MemoryManager> mem_;
    std::shared_ptr<DeviceBus> bus_;
    std::shared_ptr<MMU> mmu_;
    std::shared_ptr<ClintDevice> clint_;
    std::shared_ptr<PlicDevice> plic_;
    std::shared_ptr<JITEngine> jit_;

    std::array<uint64_t, 32> regs_{};
    uint64_t pc_{0};
    std::unordered_map<uint32_t, uint64_t> csrs_;
    PrivilegeMode mode_{PrivilegeMode::MACHINE};

    std::atomic<VCPUState> state_{VCPUState::HALTED};
    VirtMode virt_mode_{VirtMode::SOFTWARE_INTERPRETER};
    uint64_t total_cycles_{0};
    mutable std::mutex mutex_;

    bool lr_valid_{false};
    uint64_t lr_addr_{0};
};

}

#endif
