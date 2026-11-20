#include "lime/vcpu.hpp"
#include <cstring>
#include "lime/jit_x86.hpp"
#include <iostream>

namespace lime {

VCPU::VCPU(uint32_t id, std::shared_ptr<MemoryManager> mem, std::shared_ptr<DeviceBus> bus)
    : id_(id), mem_(mem), bus_(bus) {
    regs_.fill(0);
    mmu_ = std::make_shared<MMU>(mem_);
    jit_ = std::make_shared<JITEngine>();
    native_jit_ = std::make_shared<X86JIT>();
}

void VCPU::jit_execute_instruction(uint64_t inst_pc, uint32_t inst) {
    // Interpreter semantics assumed by execute_instruction(): pc_ already
    // points past the instruction being executed.
    pc_ = inst_pc + 4;
    execute_instruction(inst);
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
    if (csr_addr == 0x180 && mmu_) {
        mmu_->invalidate_tlb();
    }
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
    uint32_t raw = fetch32(curr_pc, fault);
    if (fault) {
        trigger_interrupt(12);
        return true;
    }

    if ((raw & 0x3) != 0x3) {
        uint16_t c_inst = static_cast<uint16_t>(raw & 0xFFFF);
        uint32_t exp = expand_rvc(c_inst);
        curr_inst_len_ = 2;
        pc_ += 2;
        total_cycles_++;
        execute_instruction(exp);
        return state_.load() == VCPUState::RUNNING;
    }

    curr_inst_len_ = 4;
    pc_ += 4;
    total_cycles_++;

    execute_instruction(raw);
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
        std::vector<uint8_t> block_lens;
        uint64_t scan_pc = curr_pc;
        for (int i = 0; i < 16; ++i) {
            uint32_t inst = fetch32(scan_pc, fault);
            if (fault) break;
            if ((inst & 0x3) != 0x3) {
                uint16_t c_inst = static_cast<uint16_t>(inst & 0xFFFF);
                uint32_t exp = expand_rvc(c_inst);
                block_insts.push_back(exp);
                block_lens.push_back(2);
                scan_pc += 2;
                uint32_t op = exp & 0x7F;
                if (op == 0x6F || op == 0x67 || op == 0x63 || op == 0x73) break;
            } else {
                block_insts.push_back(inst);
                block_lens.push_back(4);
                scan_pc += 4;
                uint32_t op = inst & 0x7F;
                if (op == 0x6F || op == 0x67 || op == 0x63 || op == 0x73) break;
            }
        }
        
        if (block_insts.empty()) {
            if (fault) trigger_interrupt(12);
            break;
        }

        const BasicBlock* bb = jit_->lookup_or_compile(curr_pc, block_insts, block_lens);
        if (bb) {
            // Prefer the asmjit-native machine-code block; fall back to the
            // threaded-code / interpreter path when the backend is absent or
            // declined to compile this block.
            NativeBlock* nb = native_jit_ ? native_jit_->lookup_or_compile(*bb) : nullptr;
            if (nb && nb->entry) {
                nb->exec_count++;
                nb->entry(this, regs_.data(), &pc_);
                executed += bb->ops.size();
                if (state_.load() != VCPUState::RUNNING) break;
            } else {
                if (!bb->executor) compile_block(const_cast<BasicBlock&>(*bb));
                if (!bb->executor(*this, executed)) break;
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
        uint32_t exp = expand_rvc(static_cast<uint16_t>(inst & 0xFFFF));
        if (exp != 0) execute_instruction(exp);
        return;
    }

    const uint64_t inst_base_pc = pc_ - curr_inst_len_;

    switch (opcode) {
    case 0x37:
        set_reg(rd, static_cast<uint64_t>(imm_u));
        break;

    case 0x17:
        set_reg(rd, inst_base_pc + static_cast<uint64_t>(imm_u));
        break;

    case 0x6F:
        set_reg(rd, pc_);
        pc_ = inst_base_pc + imm_j;
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
            pc_ = inst_base_pc + imm_b;
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

    case 0x0F: break;
    case 0x07: { 
        bool fault = false;
        uint64_t pa = mmu_->translate(get_reg(rs1) + imm_i, AccessType::READ, mode_, csrs_[0x180], fault);
        if (!fault) {
            if (funct3 == 3) { 
                uint64_t val = mem_->read64(pa);
                std::memcpy(&fregs_[rd], &val, sizeof(double));
            } else if (funct3 == 2) { 
                uint32_t val = mem_->read32(pa);
                float fval; std::memcpy(&fval, &val, sizeof(float));
                fregs_[rd] = static_cast<double>(fval);
            }
        }
        break;
    }
    case 0x27: { 
        bool fault = false;
        uint64_t pa = mmu_->translate(get_reg(rs1) + imm_s, AccessType::WRITE, mode_, csrs_[0x180], fault);
        if (!fault) {
            if (funct3 == 3) { 
                uint64_t val; std::memcpy(&val, &fregs_[rs2], sizeof(double));
                mem_->write64(pa, val);
            } else if (funct3 == 2) { 
                float fval = static_cast<float>(fregs_[rs2]);
                uint32_t val; std::memcpy(&val, &fval, sizeof(float));
                mem_->write32(pa, val);
            }
        }
        break;
    }
    case 0x53: { 
        uint32_t fmt = (inst >> 25) & 0x3; 
        uint32_t op = (inst >> 27) & 0x1F;
        if (fmt == 1) { 
            switch(op) {
                case 0x00: fregs_[rd] = fregs_[rs1] + fregs_[rs2]; break; 
                case 0x01: fregs_[rd] = fregs_[rs1] - fregs_[rs2]; break; 
                case 0x02: fregs_[rd] = fregs_[rs1] * fregs_[rs2]; break; 
                case 0x03: fregs_[rd] = fregs_[rs1] / fregs_[rs2]; break; 
                case 0x21: 
                    fregs_[rd] = static_cast<double>(static_cast<int32_t>(get_reg(rs1))); break;
                case 0x31: 
                    set_reg(rd, static_cast<int32_t>(fregs_[rs1])); break;
            }
        }
        break;
    }
    case 0x2F: { 
        uint32_t funct5 = (inst >> 27) & 0x1F;
        if (funct5 == 0x02) {
            execute_lr(rd, rs1, funct3);
        } else if (funct5 == 0x03) {
            execute_sc(rd, rs1, rs2, funct3);
        } else {
            bool fault = false;
            uint64_t va = get_reg(rs1);
            uint64_t pa = mmu_->translate(va, AccessType::WRITE, mode_, csrs_[0x180], fault);
            if (!fault) {
                static std::mutex amo_mutex;
                std::lock_guard<std::mutex> lock(amo_mutex);
                
                uint64_t old_val = (funct3 == 3) ? mem_->read64(pa) : static_cast<int32_t>(mem_->read32(pa));
                uint64_t src_val = get_reg(rs2);
                uint64_t new_val = 0;
                
                switch(funct5) {
                    case 0x00: new_val = old_val + src_val; break; 
                    case 0x01: new_val = src_val; break; 
                    case 0x04: new_val = old_val ^ src_val; break; 
                    case 0x08: new_val = old_val | src_val; break; 
                    case 0x0C: new_val = old_val & src_val; break; 
                    case 0x10: new_val = (static_cast<int64_t>(old_val) < static_cast<int64_t>(src_val)) ? old_val : src_val; break; 
                    case 0x14: new_val = (static_cast<int64_t>(old_val) > static_cast<int64_t>(src_val)) ? old_val : src_val; break; 
                    case 0x18: new_val = (old_val < src_val) ? old_val : src_val; break; 
                    case 0x1C: new_val = (old_val > src_val) ? old_val : src_val; break; 
                }
                
                if (funct3 == 3) mem_->write64(pa, new_val);
                else mem_->write32(pa, static_cast<uint32_t>(new_val));
                
                set_reg(rd, old_val);
            }
        }
        break;
    }

    case 0x73: {
        if (funct3 == 0) {
            uint32_t funct7 = (inst >> 25) & 0x7F;
            if (imm_i == 0) {
                csrs_[0x342] = 8;
                csrs_[0x341] = pc_ - 4;
            } else if (imm_i == 1) {
                state_ = VCPUState::HALTED;
            } else if (imm_i == 0x105) {
                state_ = VCPUState::IDLE_WAIT;
            } else if (funct7 == 0x09) { // SFENCE.VMA
                if (mmu_) {
                    uint64_t va = get_reg(rs1);
                    mmu_->invalidate_tlb(rs1 == 0 ? 0 : va);
                }
            } else if (imm_i == 0x302) { // MRET
                pc_ = get_csr(0x341);
                uint64_t mstatus = get_csr(0x300);
                uint64_t mpp = (mstatus >> 11) & 3;
                mode_ = static_cast<PrivilegeMode>(mpp);
            } else if (imm_i == 0x102) { // SRET
                pc_ = get_csr(0x141);
                uint64_t sstatus = get_csr(0x100);
                uint64_t spp = (sstatus >> 8) & 1;
                mode_ = static_cast<PrivilegeMode>(spp);
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

uint32_t VCPU::expand_rvc(uint16_t c) {
    if ((c & 0x3) == 0x3) return c;

    uint32_t op = c & 0x03;
    uint32_t funct3 = (c >> 13) & 0x07;
    uint32_t rd_prime = 8 + ((c >> 2) & 0x07);
    uint32_t rs1_prime = 8 + ((c >> 7) & 0x07);
    uint32_t rs2_prime = 8 + ((c >> 2) & 0x07);
    uint32_t rd = (c >> 7) & 0x1F;
    uint32_t rs2 = (c >> 2) & 0x1F;

    auto encode_i = [](uint32_t opcode, uint32_t rd, uint32_t funct3, uint32_t rs1, int32_t imm) -> uint32_t {
        return ((static_cast<uint32_t>(imm) & 0xFFF) << 20) | (rs1 << 15) | ((funct3 & 7) << 12) | (rd << 7) | (opcode & 0x7F);
    };
    auto encode_r = [](uint32_t opcode, uint32_t rd, uint32_t funct3, uint32_t rs1, uint32_t rs2, uint32_t funct7) -> uint32_t {
        return ((funct7 & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) | ((funct3 & 7) << 12) | (rd << 7) | (opcode & 0x7F);
    };
    auto encode_s = [](uint32_t opcode, uint32_t funct3, uint32_t rs1, uint32_t rs2, int32_t imm) -> uint32_t {
        return (((static_cast<uint32_t>(imm) >> 5) & 0x7F) << 25) | (rs2 << 20) | (rs1 << 15) | ((funct3 & 7) << 12) | ((static_cast<uint32_t>(imm) & 0x1F) << 7) | (opcode & 0x7F);
    };
    auto encode_b = [](uint32_t opcode, uint32_t funct3, uint32_t rs1, uint32_t rs2, int32_t imm) -> uint32_t {
        uint32_t u = static_cast<uint32_t>(imm);
        uint32_t bit12 = (u >> 12) & 1;
        uint32_t bit10_5 = (u >> 5) & 0x3F;
        uint32_t bit4_1 = (u >> 1) & 0xF;
        uint32_t bit11 = (u >> 11) & 1;
        return (bit12 << 31) | (bit10_5 << 25) | (rs2 << 20) | (rs1 << 15) | ((funct3 & 7) << 12) | (bit4_1 << 8) | (bit11 << 7) | (opcode & 0x7F);
    };
    auto encode_u = [](uint32_t opcode, uint32_t rd, int32_t imm) -> uint32_t {
        return (static_cast<uint32_t>(imm) & 0xFFFFF000u) | (rd << 7) | (opcode & 0x7F);
    };
    auto encode_j = [](uint32_t opcode, uint32_t rd, int32_t imm) -> uint32_t {
        uint32_t u = static_cast<uint32_t>(imm);
        uint32_t bit20 = (u >> 20) & 1;
        uint32_t bit10_1 = (u >> 1) & 0x3FF;
        uint32_t bit11 = (u >> 11) & 1;
        uint32_t bit19_12 = (u >> 12) & 0xFF;
        return (bit20 << 31) | (bit10_1 << 21) | (bit11 << 20) | (bit19_12 << 12) | (rd << 7) | (opcode & 0x7F);
    };

    switch (op) {
    case 0: { // Quadrant 0
        switch (funct3) {
        case 0: { // c.addi4spn
            uint32_t nzuimm = (((c >> 11) & 0x3) << 4) |
                              (((c >> 7) & 0xF) << 6) |
                              (((c >> 6) & 0x1) << 2) |
                              (((c >> 5) & 0x1) << 3);
            if (nzuimm == 0) return 0;
            return encode_i(0x13, rd_prime, 0, 2, static_cast<int32_t>(nzuimm));
        }
        case 2: { // c.lw
            uint32_t uimm = (((c >> 5) & 0x1) << 6) |
                            (((c >> 10) & 0x7) << 3) |
                            (((c >> 6) & 0x1) << 2);
            return encode_i(0x03, rd_prime, 2, rs1_prime, static_cast<int32_t>(uimm));
        }
        case 3: { // c.ld (RV64)
            uint32_t uimm = (((c >> 5) & 0x3) << 6) |
                            (((c >> 10) & 0x7) << 3);
            return encode_i(0x03, rd_prime, 3, rs1_prime, static_cast<int32_t>(uimm));
        }
        case 6: { // c.sw
            uint32_t uimm = (((c >> 5) & 0x1) << 6) |
                            (((c >> 10) & 0x7) << 3) |
                            (((c >> 6) & 0x1) << 2);
            return encode_s(0x23, 2, rs1_prime, rs2_prime, static_cast<int32_t>(uimm));
        }
        case 7: { // c.sd (RV64)
            uint32_t uimm = (((c >> 5) & 0x3) << 6) |
                            (((c >> 10) & 0x7) << 3);
            return encode_s(0x23, 3, rs1_prime, rs2_prime, static_cast<int32_t>(uimm));
        }
        default: return 0;
        }
    }
    case 1: { // Quadrant 1
        switch (funct3) {
        case 0: { // c.addi / c.nop
            int32_t imm = ((c >> 2) & 0x1F) | ((c & 0x1000) ? ~0x1F : 0);
            return encode_i(0x13, rd, 0, rd, imm);
        }
        case 1: { // c.addiw (RV64)
            if (rd == 0) return 0;
            int32_t imm = ((c >> 2) & 0x1F) | ((c & 0x1000) ? ~0x1F : 0);
            return encode_i(0x1B, rd, 0, rd, imm);
        }
        case 2: { // c.li
            if (rd == 0) return 0;
            int32_t imm = ((c >> 2) & 0x1F) | ((c & 0x1000) ? ~0x1F : 0);
            return encode_i(0x13, rd, 0, 0, imm);
        }
        case 3: { // c.addi16sp (rd=2) / c.lui (rd!=2,0)
            if (rd == 2) {
                int32_t imm = (((c >> 12) & 1) ? ~0x1FF : 0) |
                              (((c >> 12) & 1) << 9) |
                              (((c >> 3) & 3) << 7) |
                              (((c >> 5) & 1) << 6) |
                              (((c >> 2) & 1) << 5) |
                              (((c >> 6) & 1) << 4);
                if (imm == 0) return 0;
                return encode_i(0x13, 2, 0, 2, imm);
            } else if (rd != 0) {
                int32_t imm = (((c >> 12) & 1) ? ~0x1FFFF : 0) |
                              (((c >> 12) & 1) << 17) |
                              (((c >> 2) & 0x1F) << 12);
                if (imm == 0) return 0;
                return encode_u(0x37, rd, imm);
            }
            return 0;
        }
        case 4: {
            uint32_t sub_op = (c >> 10) & 0x3;
            if (sub_op == 0) { // c.srli
                uint32_t shamt = (((c >> 12) & 1) << 5) | ((c >> 2) & 0x1F);
                return encode_i(0x13, rd_prime, 5, rd_prime, static_cast<int32_t>(shamt));
            } else if (sub_op == 1) { // c.srai
                uint32_t shamt = (((c >> 12) & 1) << 5) | ((c >> 2) & 0x1F);
                return encode_i(0x13, rd_prime, 5, rd_prime, static_cast<int32_t>(shamt | 0x400));
            } else if (sub_op == 2) { // c.andi
                int32_t imm = ((c >> 2) & 0x1F) | ((c & 0x1000) ? ~0x1F : 0);
                return encode_i(0x13, rd_prime, 7, rd_prime, imm);
            } else {
                uint32_t sub_funct = (c >> 5) & 0x3;
                uint32_t bit12 = (c >> 12) & 1;
                if (bit12 == 0) {
                    switch (sub_funct) {
                    case 0: return encode_r(0x33, rd_prime, 0, rd_prime, rs2_prime, 0x20); // c.sub
                    case 1: return encode_r(0x33, rd_prime, 4, rd_prime, rs2_prime, 0x00); // c.xor
                    case 2: return encode_r(0x33, rd_prime, 6, rd_prime, rs2_prime, 0x00); // c.or
                    case 3: return encode_r(0x33, rd_prime, 7, rd_prime, rs2_prime, 0x00); // c.and
                    }
                } else {
                    switch (sub_funct) {
                    case 0: return encode_r(0x3B, rd_prime, 0, rd_prime, rs2_prime, 0x20); // c.subw
                    case 1: return encode_r(0x3B, rd_prime, 0, rd_prime, rs2_prime, 0x00); // c.addw
                    default: return 0;
                    }
                }
            }
            return 0;
        }
        case 5: { // c.j
            int32_t offset = (((c >> 12) & 1) ? ~0x7FF : 0) |
                             (((c >> 12) & 1) << 11) |
                             (((c >> 8) & 1) << 10) |
                             (((c >> 9) & 3) << 8) |
                             (((c >> 6) & 1) << 7) |
                             (((c >> 7) & 1) << 6) |
                             (((c >> 2) & 1) << 5) |
                             (((c >> 11) & 1) << 4) |
                             (((c >> 3) & 7) << 1);
            return encode_j(0x6F, 0, offset);
        }
        case 6: { // c.beqz
            int32_t offset = (((c >> 12) & 1) ? ~0xFF : 0) |
                             (((c >> 12) & 1) << 8) |
                             (((c >> 5) & 3) << 6) |
                             (((c >> 2) & 1) << 5) |
                             (((c >> 10) & 3) << 3) |
                             (((c >> 3) & 3) << 1);
            return encode_b(0x63, 0, rs1_prime, 0, offset);
        }
        case 7: { // c.bnez
            int32_t offset = (((c >> 12) & 1) ? ~0xFF : 0) |
                             (((c >> 12) & 1) << 8) |
                             (((c >> 5) & 3) << 6) |
                             (((c >> 2) & 1) << 5) |
                             (((c >> 10) & 3) << 3) |
                             (((c >> 3) & 3) << 1);
            return encode_b(0x63, 1, rs1_prime, 0, offset);
        }
        default: return 0;
        }
    }
    case 2: { // Quadrant 2
        switch (funct3) {
        case 0: { // c.slli
            if (rd == 0) return 0;
            uint32_t shamt = (((c >> 12) & 1) << 5) | ((c >> 2) & 0x1F);
            return encode_i(0x13, rd, 1, rd, static_cast<int32_t>(shamt));
        }
        case 2: { // c.lwsp
            if (rd == 0) return 0;
            uint32_t offset = (((c >> 2) & 3) << 6) | (((c >> 12) & 1) << 5) | (((c >> 4) & 7) << 2);
            return encode_i(0x03, rd, 2, 2, static_cast<int32_t>(offset));
        }
        case 3: { // c.ldsp (RV64)
            if (rd == 0) return 0;
            uint32_t offset = (((c >> 2) & 7) << 6) | (((c >> 12) & 1) << 5) | (((c >> 5) & 3) << 3);
            return encode_i(0x03, rd, 3, 2, static_cast<int32_t>(offset));
        }
        case 4: {
            uint32_t bit12 = (c >> 12) & 1;
            if (bit12 == 0) {
                if (rs2 == 0) {
                    if (rd == 0) return 0;
                    return encode_i(0x67, 0, 0, rd, 0); // c.jr
                } else {
                    if (rd == 0) return 0;
                    return encode_r(0x33, rd, 0, 0, rs2, 0); // c.mv
                }
            } else {
                if (rd == 0 && rs2 == 0) {
                    return 0x00100073; // c.ebreak
                } else if (rs2 == 0) {
                    if (rd == 0) return 0;
                    return encode_i(0x67, 1, 0, rd, 0); // c.jalr
                } else {
                    if (rd == 0) return 0;
                    return encode_r(0x33, rd, 0, rd, rs2, 0); // c.add
                }
            }
        }
        case 6: { // c.swsp
            uint32_t offset = (((c >> 7) & 3) << 6) | (((c >> 9) & 0xF) << 2);
            return encode_s(0x23, 2, 2, rs2, static_cast<int32_t>(offset));
        }
        case 7: { // c.sdsp (RV64)
            uint32_t offset = (((c >> 7) & 7) << 6) | (((c >> 10) & 7) << 3);
            return encode_s(0x23, 3, 2, rs2, static_cast<int32_t>(offset));
        }
        default: return 0;
        }
    }
    default: return 0;
    }
}

bool VCPU::execute_compressed(uint16_t inst) {
    uint32_t exp = expand_rvc(inst);
    if (exp == 0) return false;
    uint8_t old_len = curr_inst_len_;
    curr_inst_len_ = 2;
    execute_instruction(exp);
    curr_inst_len_ = old_len;
    return true;
}

bool VCPU::execute_block_fast(const BasicBlock* bb, size_t& executed) {
#if !defined(__GNUC__) && !defined(__clang__)
    // Portable path for compilers without GCC computed-goto extensions.
    for (const MicroOp& op : bb->ops) {
        uint64_t inst_pc = pc_;
        curr_inst_len_ = op.len;
        pc_ += op.len;
        execute_instruction(op.raw_inst);
        executed++;
        if (state_.load() != VCPUState::RUNNING) return false;
        (void)inst_pc;
    }
    return true;
#else
    static const void* dispatch_table[128];
    static bool table_initialized = false;
    if (!table_initialized) {
        for (int i = 0; i < 128; i++) dispatch_table[i] = &&OP_FALLBACK;
        dispatch_table[0x03] = &&OP_LOAD;
        dispatch_table[0x23] = &&OP_STORE;
        dispatch_table[0x13] = &&OP_ALUI;
        dispatch_table[0x33] = &&OP_ALUR;
        dispatch_table[0x37] = &&OP_LUI;
        dispatch_table[0x17] = &&OP_AUIPC;
        table_initialized = true;
    }

    const MicroOp* op = bb->ops.data();
    const MicroOp* end = op + bb->ops.size();

    if (op == end) return true;
    goto *dispatch_table[op->opcode];

OP_LUI:
    curr_inst_len_ = op->len;
    pc_ += op->len;
    if (op->rd != 0) regs_[op->rd] = static_cast<int64_t>(op->imm & 0xFFFFF000);
    op++; executed++;
    if (op == end) return true;
    goto *dispatch_table[op->opcode];

OP_AUIPC:
    curr_inst_len_ = op->len;
    if (op->rd != 0) regs_[op->rd] = pc_ + static_cast<int64_t>(op->imm & 0xFFFFF000);
    pc_ += op->len;
    op++; executed++;
    if (op == end) return true;
    goto *dispatch_table[op->opcode];

OP_ALUI: {
    curr_inst_len_ = op->len;
    pc_ += op->len;
    uint64_t val1 = get_reg(op->rs1);
    uint32_t funct3 = (op->raw_inst >> 12) & 0x7;
    uint64_t res = 0;
    switch(funct3) {
        case 0: res = val1 + op->imm; break;
        case 4: res = val1 ^ op->imm; break;
        case 6: res = val1 | op->imm; break;
        case 7: res = val1 & op->imm; break;
        default: execute_instruction(op->raw_inst); pc_ -= op->len; res = get_reg(op->rd); break;
    }
    if (op->rd != 0) regs_[op->rd] = res;
    op++; executed++;
    if (op == end) return true;
    goto *dispatch_table[op->opcode];
}

OP_ALUR:
    curr_inst_len_ = op->len;
    pc_ += op->len;
    execute_instruction(op->raw_inst);
    pc_ -= op->len; 
    op++; executed++;
    if (op == end || state_.load() != VCPUState::RUNNING) return state_.load() == VCPUState::RUNNING;
    goto *dispatch_table[op->opcode];

OP_LOAD:
OP_STORE:
OP_FALLBACK:
    curr_inst_len_ = op->len;
    pc_ += op->len;
    execute_instruction(op->raw_inst);
    pc_ -= op->len; 
    op++; executed++;
    if (op == end || state_.load() != VCPUState::RUNNING) return state_.load() == VCPUState::RUNNING;
    goto *dispatch_table[op->opcode];
#endif
}

void VCPU::compile_block(BasicBlock& bb) {
    BasicBlock* bb_ptr = &bb;
    bb.executor = [this, bb_ptr](VCPU& cpu, size_t& executed) -> bool {
        return cpu.execute_block_fast(bb_ptr, executed);
    };
}

}



