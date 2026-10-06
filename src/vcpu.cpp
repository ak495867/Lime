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
        
        std::vector<uint32_t> block_insts;
        uint64_t scan_pc = curr_pc;
        for (int i=0; i<16; ++i) {
            uint32_t inst = fetch32(scan_pc, fault);
            if (fault) break;
            block_insts.push_back(inst);
            scan_pc += 4;
            uint32_t op = inst & 0x7F;
            if (op == 0x6F || op == 0x67 || op == 0x63 || op == 0x73) break;
        }
        
        if (block_insts.empty()) {
            if (fault) trigger_interrupt(12);
            break;
        }

        const BasicBlock* bb = jit_->lookup_or_compile(curr_pc, block_insts);
        if (bb) {
            for (const auto& uop : bb->ops) {
                pc_ += 4;
                execute_instruction(uop.raw_inst);
                executed++;
                if (state_.load() != VCPUState::RUNNING) break;
            }
        } else {
            if (!step()) break;
            executed++;
        }
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


    if ((inst & 0x3) != 0x3) {
        execute_compressed(static_cast<uint16_t>(inst & 0xFFFF));
        return;
    }

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

    case 0x0F: {



        if ((rs2 == 0) && (funct3 == 0x02 || funct3 == 0x03)) {
            execute_lr(rd, rs1, funct3);
        } else if (funct3 == 0x02 || funct3 == 0x03) {
            execute_sc(rd, rs1, rs2, funct3);
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

void VCPU::execute_lr(uint32_t rd, uint32_t rs1, uint32_t funct3) {
    bool fault = false;
    uint64_t rs1_val = get_reg(rs1);
    uint64_t pa = mmu_->translate(rs1_val, AccessType::READ, mode_, get_csr(0x180), fault);
    if (fault) {
        trigger_interrupt(13);
        return;
    }
    uint64_t val = 0;
    switch (funct3) {
    case 0x02: val = mem_->read32(pa); break;  
    case 0x03: val = mem_->read64(pa); break;  
    default: return;
    }
    set_reg(rd, val);
    lr_valid_ = true;
    lr_addr_ = pa;
}

void VCPU::execute_sc(uint32_t rd, uint32_t rs1, uint32_t rs2, uint32_t funct3) {
    bool fault = false;
    uint64_t rs1_val = get_reg(rs1);
    uint64_t pa = mmu_->translate(rs1_val, AccessType::WRITE, mode_, get_csr(0x180), fault);
    if (fault) {
        trigger_interrupt(15);
        set_reg(rd, 1);  
        return;
    }
    if (lr_valid_ && lr_addr_ == pa) {
        uint64_t val2 = get_reg(rs2);
        switch (funct3) {
        case 0x02: mem_->write32(pa, static_cast<uint32_t>(val2)); break;  
        case 0x03: mem_->write64(pa, val2); break;  
        default: break;
        }
        set_reg(rd, 0);  
        lr_valid_ = false;  
    } else {
        set_reg(rd, 1);  
    }
}

bool VCPU::execute_compressed(uint16_t inst) {
    uint32_t opcode = (inst >> 13) & 0x07;
    uint32_t rd_prime = (inst >> 7) & 0x07;
    uint32_t rs1_prime = (inst >> 7) & 0x07;
    uint32_t rs2_prime = (inst >> 2) & 0x07;

    switch (opcode) {
    case 0b000: {  
        uint32_t imm3 = (inst >> 4) & 0x07;
        uint32_t imm2 = (inst >> 6) & 0x01;
        uint32_t imm1 = (inst >> 5) & 0x01;
        uint32_t imm6 = (inst >> 11) & 0x01;
        int32_t nzimm = (imm6 << 5) | (imm3 << 2) | (imm2 << 1) | imm1;
        if (rd_prime == 0) break;  
        int32_t rd = rd_prime + (rd_prime < 2 ? 8 : 0);
        int32_t rs1 = rs1_prime + (rs1_prime < 2 ? 8 : 0);
        if (rd == 2 && rs1 == 2) {

            if (nzimm == 0) break;
            set_reg(rd, get_reg(2) + static_cast<uint64_t>(nzimm));
        } else {
            set_reg(rd, get_reg(rs1) + static_cast<uint64_t>(nzimm));
        }
        break;
    }
    case 0b001: {  
        uint32_t imm5 = (inst >> 5) & 0x01;
        uint32_t imm4 = (inst >> 10) & 0x01;
        uint32_t imm6 = (inst >> 6) & 0x01;
        int32_t offset = (imm6 << 5) | (imm5 << 4) | (imm4 << 3) | ((inst >> 7) & 0x07);
        if (offset & 0x40) offset |= ~0x7F;  
        uint64_t addr = get_reg(rs1_prime + 8) + static_cast<uint64_t>(offset);
        bool fault = false;
        uint64_t pa = mmu_->translate(addr, AccessType::READ, mode_, get_csr(0x180), fault);
        if (!fault) {
            set_reg(rd_prime + 8, mem_->read32(pa));
        }
        break;
    }
    case 0b100: {  
        uint32_t funct2 = (inst >> 10) & 0x03;
        if (funct2 == 0b00 || funct2 == 0b01) {  
            uint32_t imm5 = (inst >> 5) & 0x01;
            uint32_t imm4 = (inst >> 6) & 0x01;
            uint32_t imm6 = (inst >> 10) & 0x01;
            int32_t offset = (imm6 << 5) | (imm5 << 4) | (imm4 << 3) | ((inst >> 7) & 0x07);
            if (offset & 0x40) offset |= ~0x7F;
            uint32_t rs2 = rs2_prime + 8;
            uint64_t addr = get_reg(rs1_prime + 8) + static_cast<uint64_t>(offset);
            bool fault = false;
            uint64_t pa = mmu_->translate(addr, AccessType::WRITE, mode_, get_csr(0x180), fault);
            if (!fault) {
                mem_->write32(pa, static_cast<uint32_t>(get_reg(rs2)));
            }
        } else if (funct2 == 0b10 || funct2 == 0b11) {  
            uint32_t bimm4 = (inst >> 6) & 0x01;
            uint32_t bimm3 = (inst >> 5) & 0x01;
            uint32_t bimm6 = (inst >> 10) & 0x01;
            uint32_t bimm5 = (inst >> 11) & 0x01;
            int32_t boffset = (bimm6 << 5) | (bimm4 << 4) | (bimm3 << 3) | (bimm5 << 2) | ((inst >> 7) & 0x03);
            if (boffset & 0x20) boffset |= ~0x3F;  
            uint64_t rs1_val = get_reg(rs1_prime + 8);
            bool take = (funct2 == 0b10) ? (rs1_val == 0) : (rs1_val != 0);
            if (take) {
                pc_ = (pc_ - 2) + static_cast<uint64_t>(boffset);
            }
        }
        break;
    }
    case 0b101: {  
        uint32_t imm12 = (inst >> 12) & 0x01;
        uint32_t imm11 = (inst >> 11) & 0x01;
        uint32_t imm4 = (inst >> 4) & 0x01;
        uint32_t imm9 = (inst >> 3) & 0x01;
        uint32_t imm8 = (inst >> 10) & 0x01;
        uint32_t imm7 = (inst >> 9) & 0x01;
        uint32_t imm6 = (inst >> 8) & 0x01;
        uint32_t imm5 = (inst >> 7) & 0x01;
        int32_t imm = (imm12 << 11) | (imm11 << 10) | (imm4 << 9) | (imm9 << 8) |
                      (imm8 << 7) | (imm7 << 6) | (imm6 << 5) | (imm5 << 4) |
                      ((inst >> 6) & 0x01) << 3 | ((inst >> 10) & 0x01) << 2 |
                      ((inst >> 5) & 0x01) << 1 | ((inst >> 1) & 0x01);
        if (imm & 0x800) imm |= ~0xFFF;  
        if (opcode == 0b101 && rd_prime == 1) {  
            set_reg(1, pc_ + 2);
        }
        pc_ = (pc_ - 2) + static_cast<uint64_t>(imm);
        break;
    }
    case 0b110: {  
        int32_t imm5 = static_cast<int32_t>(inst) >> 12;
        uint32_t funct2 = (inst >> 10) & 0x03;
        if (rd_prime == 0) {  
            break;
        }
        uint32_t rd = rd_prime + (rd_prime < 2 ? 8 : 0);
        if (funct2 == 0b00) {  
            int32_t nzimm = ((inst >> 12) & 0x01) ? ((inst >> 12) | ~0x1F) : ((inst >> 12) & 0x1F);
            nzimm = (nzimm & 0x10) ? (nzimm | ~0x1F) : nzimm;  
            if (rd == 2 && rs1_prime == 2) {

                uint32_t nzimm16sp = ((inst >> 12) & 0x01) << 5 | ((inst >> 6) & 0x01) << 4 |
                                     ((inst >> 5) & 0x01) << 3 | ((inst >> 3) & 0x01) << 2 |
                                     ((inst >> 4) & 0x01) << 1 | ((inst >> 2) & 0x01);
                if (nzimm16sp & 0x20) nzimm16sp |= ~0x3F;
                set_reg(rd, get_reg(2) + static_cast<uint64_t>(nzimm16sp));
            } else {
                set_reg(rd, get_reg(rd) + static_cast<uint64_t>(imm5));
            }
        } else if (funct2 == 0b10) {  
            uint32_t shamt = ((inst >> 11) & 0x01) << 5 | ((inst >> 7) & 0x1F);
            uint32_t rs2 = rs2_prime + 8;
            if ((inst >> 12) & 0x01) {
                set_reg(rd, static_cast<uint64_t>(static_cast<int64_t>(get_reg(rs2)) >> shamt));
            } else {
                set_reg(rd, get_reg(rs2) >> shamt);
            }
        } else if (funct2 == 0b11) {  
            uint32_t rs2 = rs2_prime + 8;
            int32_t simm5 = ((inst >> 12) & 0x01) ? ((inst >> 12) | ~0x1F) : ((inst >> 12) & 0x1F);
            set_reg(rd, get_reg(rs2) & static_cast<uint64_t>(simm5));
        } else if (funct2 == 0b01) {  
            uint32_t rs2 = rs2_prime + 8;
            uint32_t funct3_c = (inst >> 10) & 0x03;
            switch (funct3_c) {
            case 0b00: set_reg(rd, get_reg(rd) - get_reg(rs2)); break;  
            case 0b01: set_reg(rd, get_reg(rd) ^ get_reg(rs2)); break;  
            case 0b10: set_reg(rd, get_reg(rd) | get_reg(rs2)); break;  
            case 0b11: set_reg(rd, get_reg(rd) & get_reg(rs2)); break;  
            }
        }
        break;
    }
    case 0b111: {  
        uint32_t funct3_c = (inst >> 10) & 0x03;
        uint32_t rs1 = rs1_prime + 8;
        uint32_t rs2 = rs2_prime + 8;
        switch (funct3_c) {
        case 0b00: set_reg(rs1_prime + 8, get_reg(rs1) - get_reg(rs2)); break;
        case 0b01: set_reg(rs1_prime + 8, get_reg(rs1) ^ get_reg(rs2)); break;
        case 0b10: set_reg(rs1_prime + 8, get_reg(rs1) | get_reg(rs2)); break;
        case 0b11: set_reg(rs1_prime + 8, get_reg(rs1) & get_reg(rs2)); break;
        }
        break;
    }
    case 0b1000: {  
        uint32_t funct3_c = (inst >> 12) & 0x07;
        if (funct3_c == 0b000 && rs2_prime == 0) {  
            pc_ = get_reg(rs1_prime + 8);
        } else if (funct3_c == 0b000) {  
            set_reg(rd_prime + 8, get_reg(rs2_prime + 8));
        } else if (funct3_c == 0b001 && rs2_prime == 0) {  
            uint64_t ra = get_reg(1);
            pc_ = get_reg(rs1_prime + 8);
            set_reg(1, pc_ + 2);
        } else if (funct3_c == 0b001) {  
            set_reg(rd_prime + 8, get_reg(rd_prime + 8) + get_reg(rs2_prime + 8));
        }
        break;
    }
    case 0b1001: {  
        if (rd_prime == 0 && rs1_prime == 0 && rs2_prime == 0) {
            state_ = VCPUState::HALTED;
        }
        break;
    }
    default:
        break;
    }

    pc_ += 2;  
    return true;
}

}
