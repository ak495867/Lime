#include "lime/vcpu.hpp"
#include <iostream>

namespace lime {

VCPU::VCPU(uint32_t id, std::shared_ptr<MemoryManager> mem, std::shared_ptr<DeviceBus> bus)
    : id_(id), mem_(mem), bus_(bus) {
    regs_.fill(0);
    mmu_ = std::make_shared<MMU>(mem_);
    jit_ = std::make_shared<JITEngine>();
}

void VCPU::attach_clint(std::shared_ptr<ClintDevice> clint) {
    clint_ = clint;
}

void VCPU::attach_plic(std::shared_ptr<PlicDevice> plic) {
    plic_ = plic;
}

void VCPU::reset(uint64_t entry_point) {
    std::lock_guard<std::mutex> lock(mutex_);
    regs_.fill(0);
    pc_ = entry_point;
    total_cycles_ = 0;
    state_ = VCPUState::RUNNING;
    mode_ = PrivilegeMode::MACHINE;
    csrs_[0x300] = 0;
    csrs_[0x305] = 0;
    csrs_[0x341] = 0;
    csrs_[0x342] = 0;
    csrs_[0x180] = 0;
}

uint64_t VCPU::get_reg(size_t idx) const {
    if (idx >= 32) return 0;
    if (idx == 0) return 0;
    return regs_[idx];
}

void VCPU::set_reg(size_t idx, uint64_t val) {
    if (idx > 0 && idx < 32) {
        regs_[idx] = val;
    }
}

uint64_t VCPU::get_pc() const {
    return pc_;
}

void VCPU::set_pc(uint64_t pc) {
    pc_ = pc;
}

uint64_t VCPU::get_csr(uint32_t csr_addr) const {
    auto it = csrs_.find(csr_addr);
    if (it != csrs_.end()) {
        return it->second;
    }
    return 0;
}

void VCPU::set_csr(uint32_t csr_addr, uint64_t val) {
    csrs_[csr_addr] = val;
}

VCPUState VCPU::state() const {
    return state_.load();
}

void VCPU::set_state(VCPUState state) {
    state_ = state;
}

PrivilegeMode VCPU::privilege_mode() const {
    return mode_;
}

void VCPU::set_privilege_mode(PrivilegeMode mode) {
    mode_ = mode;
}

uint64_t VCPU::total_cycles() const {
    return total_cycles_;
}

uint32_t VCPU::id() const {
    return id_;
}

void VCPU::set_virt_mode(VirtMode mode) {
    virt_mode_ = mode;
}

VirtMode VCPU::virt_mode() const {
    return virt_mode_;
}

void VCPU::trigger_interrupt(uint64_t cause) {
    std::lock_guard<std::mutex> lock(mutex_);
    csrs_[0x341] = pc_;
    csrs_[0x342] = cause;
    uint64_t handler = csrs_[0x305];
    if (handler != 0) {
        pc_ = handler;
    }
    state_ = VCPUState::RUNNING;
}

uint32_t VCPU::fetch32(uint64_t addr, bool& fault) {
    fault = false;
    uint64_t satp = get_csr(0x180);
    uint64_t pa = mmu_->translate(addr, AccessType::FETCH, mode_, satp, fault);
    if (fault) return 0;

    if (bus_->find_device(pa)) {
        return bus_->read(pa, 4);
    }
    return mem_->read32(pa);
}

bool VCPU::step() {
    if (state_.load() != VCPUState::RUNNING) {
        return false;
    }

    if (clint_) {
        clint_->tick();
        if (clint_->timer_interrupt_pending()) {
            trigger_interrupt(0x8000000000000007ULL);
        }
    }

    if (plic_ && plic_->external_interrupt_pending()) {
        trigger_interrupt(0x800000000000000BULL);
    }

    uint64_t curr_pc = pc_;
    bool fault = false;
    uint32_t inst = fetch32(curr_pc, fault);
    if (fault) {
        trigger_interrupt(12);
        return true;
    }

    pc_ += 4;
    total_cycles_++;

    execute_instruction(inst);
    return state_.load() == VCPUState::RUNNING;
}

size_t VCPU::run_cycles(size_t max_cycles) {
    size_t executed = 0;
    while (executed < max_cycles && state_.load() == VCPUState::RUNNING) {
        if (!step()) break;
        executed++;
    }
    return executed;
}

void VCPU::execute_instruction(uint32_t inst) {
    uint32_t opcode = inst & 0x7F;
    uint32_t rd = (inst >> 7) & 0x1F;
    uint32_t funct3 = (inst >> 12) & 0x07;
    uint32_t rs1 = (inst >> 15) & 0x1F;
    uint32_t rs2 = (inst >> 20) & 0x1F;
    uint32_t funct7 = (inst >> 25) & 0x7F;

    int32_t imm_i = static_cast<int32_t>(inst) >> 20;
    int32_t imm_s = ((static_cast<int32_t>(inst) >> 25) << 5) | ((inst >> 7) & 0x1F);
    int32_t imm_b = ((static_cast<int32_t>(inst) >> 31) << 12) |
                    (((inst >> 25) & 0x3F) << 5) |
                    (((inst >> 8) & 0x0F) << 1) |
                    (((inst >> 7) & 0x01) << 11);
    int32_t imm_u = static_cast<int32_t>(inst & 0xFFFFF000);
    int32_t imm_j = ((static_cast<int32_t>(inst) >> 31) << 20) |
                    (((inst >> 12) & 0xFF) << 12) |
                    (((inst >> 20) & 0x01) << 11) |
                    (((inst >> 21) & 0x3FF) << 1);

    uint64_t satp = get_csr(0x180);

    switch (opcode) {
    case 0x37:
        set_reg(rd, static_cast<uint64_t>(imm_u));
        break;

    case 0x17:
        set_reg(rd, (pc_ - 4) + static_cast<uint64_t>(imm_u));
        break;

    case 0x6F:
        set_reg(rd, pc_);
        pc_ = (pc_ - 4) + imm_j;
        break;

    case 0x67: {
        uint64_t target = (get_reg(rs1) + imm_i) & ~1ULL;
        set_reg(rd, pc_);
        pc_ = target;
        break;
    }

    case 0x63: {
        uint64_t val1 = get_reg(rs1);
        uint64_t val2 = get_reg(rs2);
        bool take = false;
        switch (funct3) {
        case 0: take = (val1 == val2); break;
        case 1: take = (val1 != val2); break;
        case 4: take = (static_cast<int64_t>(val1) < static_cast<int64_t>(val2)); break;
        case 5: take = (static_cast<int64_t>(val1) >= static_cast<int64_t>(val2)); break;
        case 6: take = (val1 < val2); break;
        case 7: take = (val1 >= val2); break;
        }
        if (take) {
            pc_ = (pc_ - 4) + imm_b;
        }
        break;
    }

    case 0x03: {
        uint64_t va = get_reg(rs1) + imm_i;
        bool fault = false;
        uint64_t pa = mmu_->translate(va, AccessType::READ, mode_, satp, fault);
        if (fault) {
            trigger_interrupt(13);
            break;
        }
        auto dev = bus_->find_device(pa);
        if (dev) {
            uint32_t val = dev->read(pa - dev->base_address(), 1 << funct3);
            set_reg(rd, val);
        } else {
            switch (funct3) {
            case 0: set_reg(rd, static_cast<int8_t>(mem_->read8(pa))); break;
            case 1: set_reg(rd, static_cast<int16_t>(mem_->read16(pa))); break;
            case 2: set_reg(rd, static_cast<int32_t>(mem_->read32(pa))); break;
            case 3: set_reg(rd, mem_->read64(pa)); break;
            case 4: set_reg(rd, mem_->read8(pa)); break;
            case 5: set_reg(rd, mem_->read16(pa)); break;
            case 6: set_reg(rd, mem_->read32(pa)); break;
            }
        }
        break;
    }

    case 0x23: {
        uint64_t va = get_reg(rs1) + imm_s;
        uint64_t val = get_reg(rs2);
        bool fault = false;
        uint64_t pa = mmu_->translate(va, AccessType::WRITE, mode_, satp, fault);
        if (fault) {
            trigger_interrupt(15);
            break;
        }
        auto dev = bus_->find_device(pa);
        if (dev) {
            dev->write(pa - dev->base_address(), static_cast<uint32_t>(val), 1 << funct3);
        } else {
            switch (funct3) {
            case 0: mem_->write8(pa, static_cast<uint8_t>(val)); break;
            case 1: mem_->write16(pa, static_cast<uint16_t>(val)); break;
            case 2: mem_->write32(pa, static_cast<uint32_t>(val)); break;
            case 3: mem_->write64(pa, val); break;
            }
        }
        break;
    }

    case 0x13: {
        uint64_t val1 = get_reg(rs1);
        uint64_t val2 = static_cast<uint64_t>(imm_i);
        switch (funct3) {
        case 0: set_reg(rd, val1 + val2); break;
        case 1: set_reg(rd, val1 << (imm_i & 0x3F)); break;
        case 2: set_reg(rd, (static_cast<int64_t>(val1) < imm_i) ? 1 : 0); break;
        case 3: set_reg(rd, (val1 < static_cast<uint64_t>(imm_i)) ? 1 : 0); break;
        case 4: set_reg(rd, val1 ^ val2); break;
        case 5:
            if (funct7 & 0x20) {
                set_reg(rd, static_cast<uint64_t>(static_cast<int64_t>(val1) >> (imm_i & 0x3F)));
            } else {
                set_reg(rd, val1 >> (imm_i & 0x3F));
            }
            break;
        case 6: set_reg(rd, val1 | val2); break;
        case 7: set_reg(rd, val1 & val2); break;
        }
        break;
    }

    case 0x33: {
        uint64_t val1 = get_reg(rs1);
        uint64_t val2 = get_reg(rs2);
        if (funct7 == 0x01) {
            switch (funct3) {
            case 0: set_reg(rd, val1 * val2); break;
            case 4: set_reg(rd, (val2 != 0) ? (static_cast<int64_t>(val1) / static_cast<int64_t>(val2)) : -1); break;
            case 6: set_reg(rd, (val2 != 0) ? (val1 / val2) : -1ULL); break;
            }
        } else {
            switch (funct3) {
            case 0: set_reg(rd, (funct7 & 0x20) ? (val1 - val2) : (val1 + val2)); break;
            case 1: set_reg(rd, val1 << (val2 & 0x3F)); break;
            case 2: set_reg(rd, (static_cast<int64_t>(val1) < static_cast<int64_t>(val2)) ? 1 : 0); break;
            case 3: set_reg(rd, (val1 < val2) ? 1 : 0); break;
            case 4: set_reg(rd, val1 ^ val2); break;
            case 5:
                if (funct7 & 0x20) {
                    set_reg(rd, static_cast<uint64_t>(static_cast<int64_t>(val1) >> (val2 & 0x3F)));
                } else {
                    set_reg(rd, val1 >> (val2 & 0x3F));
                }
                break;
            case 6: set_reg(rd, val1 | val2); break;
            case 7: set_reg(rd, val1 & val2); break;
            }
        }
        break;
    }

    case 0x73: {
        if (funct3 == 0) {
            if (imm_i == 0) {
                csrs_[0x342] = 8;
                csrs_[0x341] = pc_ - 4;
            } else if (imm_i == 1) {
                state_ = VCPUState::HALTED;
            } else if (imm_i == 0x105) {
                state_ = VCPUState::IDLE_WAIT;
            }
        } else {
            uint32_t csr_addr = imm_i & 0xFFF;
            uint64_t old_val = get_csr(csr_addr);
            uint64_t val1 = get_reg(rs1);
            set_reg(rd, old_val);
            switch (funct3) {
            case 1: set_csr(csr_addr, val1); break;
            case 2: set_csr(csr_addr, old_val | val1); break;
            case 3: set_csr(csr_addr, old_val & ~val1); break;
            }
        }
        break;
    }

    default:
        break;
    }
}

}
