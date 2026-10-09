#include "lime/x86_cpu.hpp"

#include <cstring>

namespace lime {

X86CPUDecoder::X86CPUDecoder(std::shared_ptr<MemoryManager> mem, std::shared_ptr<DeviceBus> bus)
    : mem_(mem), bus_(bus) {
    gprs_.fill(0);
}

void X86CPUDecoder::reset(uint64_t entry_point) {
    gprs_.fill(0);
    rip_ = entry_point;
    rflags_ = 0x02;
    cr0_ = 0x60000010;
    cr3_ = 0;
    cr4_ = 0;
    mode_ = CPUMode::LONG_64BIT;
}

uint64_t X86CPUDecoder::get_gpr(size_t index) const {
    if (index < 16) return gprs_[index];
    return 0;
}

void X86CPUDecoder::set_gpr(size_t index, uint64_t val) {
    if (index < 16) {
        gprs_[index] = val;
    }
}

uint64_t X86CPUDecoder::get_rip() const {
    return rip_;
}

void X86CPUDecoder::set_rip(uint64_t rip) {
    rip_ = rip;
}

CPUMode X86CPUDecoder::mode() const {
    return mode_;
}

void X86CPUDecoder::set_mode(CPUMode m) {
    mode_ = m;
}

// ---------------------------------------------------------------------------
// Flags
// ---------------------------------------------------------------------------
void X86CPUDecoder::set_flag(int bit, bool value) {
    if (value) {
        rflags_ |= (1ULL << bit);
    } else {
        rflags_ &= ~(1ULL << bit);
    }
}

bool X86CPUDecoder::get_flag(int bit) const {
    return ((rflags_ >> bit) & 1ULL) != 0;
}

static inline uint64_t size_mask(uint32_t size) {
    if (size >= 8) return ~0ULL;
    return (1ULL << (size * 8)) - 1ULL;
}

void X86CPUDecoder::update_flags_logic(uint64_t result, uint32_t size) {
    result &= size_mask(size);
    set_flag(kZF, result == 0);
    set_flag(kSF, (result >> (size * 8 - 1)) & 1ULL);
    set_flag(kPF, (__builtin_popcount(static_cast<uint8_t>(result)) % 2) == 0);
    set_flag(kCF, false);
    set_flag(kOF, false);
}

void X86CPUDecoder::update_flags_arith(uint64_t result, uint64_t lhs, uint64_t rhs,
                                        uint32_t size, bool subtract) {
    const uint64_t mask = size_mask(size);
    const uint32_t bits = size * 8;
    result &= mask;
    lhs &= mask;
    rhs &= mask;

    set_flag(kZF, result == 0);
    set_flag(kSF, (result >> (bits - 1)) & 1ULL);
    set_flag(kPF, (__builtin_popcount(static_cast<uint8_t>(result)) % 2) == 0);

    if (subtract) {
        set_flag(kCF, lhs < rhs);
        const uint64_t sign_bit = 1ULL << (bits - 1);
        set_flag(kOF, (((lhs ^ rhs) & (lhs ^ result)) & sign_bit) != 0);
    } else {
        set_flag(kCF, result < lhs);
        const uint64_t sign_bit = 1ULL << (bits - 1);
        set_flag(kOF, (((lhs ^ result) & (rhs ^ result)) & sign_bit) != 0);
    }
}

void X86CPUDecoder::update_flags_incdec(uint64_t result, uint64_t operand, uint32_t size,
                                        bool increment) {
    const uint64_t mask = size_mask(size);
    const uint32_t bits = size * 8;
    result &= mask;
    operand &= mask;

    set_flag(kZF, result == 0);
    set_flag(kSF, (result >> (bits - 1)) & 1ULL);
    set_flag(kPF, (__builtin_popcount(static_cast<uint8_t>(result)) % 2) == 0);

    const uint64_t sign_bit = 1ULL << (bits - 1);
    if (increment) {
        set_flag(kOF, (result == sign_bit) && (operand == (sign_bit - 1)));
    } else {
        set_flag(kOF, (result == (sign_bit - 1)) && (operand == sign_bit));
    }
    // CF is preserved by INC/DEC.
}

void X86CPUDecoder::update_flags_shift(uint64_t result, uint32_t size, uint32_t count) {
    if (count == 0) return;
    const uint32_t bits = size * 8;
    set_flag(kZF, (result & size_mask(size)) == 0);
    set_flag(kSF, ((result & size_mask(size)) >> (bits - 1)) & 1ULL);
    set_flag(kPF, (__builtin_popcount(static_cast<uint8_t>(result)) % 2) == 0);
}

// ---------------------------------------------------------------------------
// Sized register / memory access
// ---------------------------------------------------------------------------
uint64_t X86CPUDecoder::read_reg_sized(uint32_t index, uint32_t size) const {
    uint64_t value = (index < 16) ? gprs_[index] : 0;
    return value & size_mask(size);
}

void X86CPUDecoder::write_reg_sized(uint32_t index, uint64_t value, uint32_t size) {
    if (index >= 16) return;
    switch (size) {
    case 1:
        gprs_[index] = (gprs_[index] & ~0xFFULL) | (value & 0xFFULL);
        break;
    case 2:
        gprs_[index] = (gprs_[index] & ~0xFFFFULL) | (value & 0xFFFFULL);
        break;
    case 4:
        gprs_[index] = value & 0xFFFFFFFFULL;  // 32-bit writes zero-extend
        break;
    default:
        gprs_[index] = value;
        break;
    }
}

uint8_t X86CPUDecoder::reg8_index(uint32_t rm, bool rex_active) const {
    // Without REX, encodings 4..7 address AH/CH/DH/BH (registers 4..7).
    // With REX they become SPL/BPL/SIL/DIL — handled by write_reg_sized's
    // low-byte semantics on the same GPR indices, so both map to 4..7 here.
    (void)rex_active;
    return static_cast<uint8_t>(rm & 0x7);
}

uint64_t X86CPUDecoder::read_mem_val(uint64_t addr, uint32_t size) {
    switch (size) {
    case 1: return mem_->read8(addr);
    case 2: return mem_->read16(addr);
    case 4: return mem_->read32(addr);
    default: return mem_->read64(addr);
    }
}

void X86CPUDecoder::write_mem_val(uint64_t addr, uint64_t value, uint32_t size) {
    switch (size) {
    case 1: mem_->write8(addr, static_cast<uint8_t>(value)); break;
    case 2: mem_->write16(addr, static_cast<uint16_t>(value)); break;
    case 4: mem_->write32(addr, static_cast<uint32_t>(value)); break;
    default: mem_->write64(addr, value); break;
    }
}

// ---------------------------------------------------------------------------
// ModRM / SIB decoding (flat 64-bit linear addressing)
// ---------------------------------------------------------------------------
bool X86CPUDecoder::decode_modrm(uint8_t modrm, uint32_t& reg, uint32_t& rm, bool& is_mem,
                                 uint64_t& addr, uint32_t imm_size) {
    const uint32_t mod = (modrm >> 6) & 0x3;
    const uint32_t rm_low = modrm & 0x7;
    reg = ((modrm >> 3) & 0x7) | ((rex_ & 0x04) ? 8 : 0);
    rm = rm_low | ((rex_ & 0x01) ? 8 : 0);
    is_mem = (mod != 3);
    addr = 0;

    if (!is_mem) return true;

    if (rm_low == 0x4) {  // SIB byte present
        const uint8_t sib = fetch8();
        const uint32_t scale = 1u << ((sib >> 6) & 0x3);
        const uint32_t index = ((sib >> 3) & 0x7) | ((rex_ & 0x02) ? 8 : 0);
        const uint32_t base_low = sib & 0x7;
        const uint32_t base = base_low | ((rex_ & 0x01) ? 8 : 0);

        uint64_t base_val = 0;
        if (!(mod == 0 && base_low == 0x5)) {
            base_val = get_gpr(base);
        }
        uint64_t index_val = (index == 4) ? 0 : get_gpr(index);  // rsp cannot be an index

        int64_t disp = 0;
        if (mod == 0 && base_low == 0x5) {
            disp = static_cast<int32_t>(fetch32v());
        } else if (mod == 1) {
            disp = static_cast<int8_t>(fetch8());
        } else if (mod == 2) {
            disp = static_cast<int32_t>(fetch32v());
        }
        addr = base_val + index_val * scale + static_cast<uint64_t>(disp);
        return true;
    }

    if (mod == 0 && rm_low == 0x5) {  // RIP-relative in 64-bit mode / absolute disp32
        int32_t disp = static_cast<int32_t>(fetch32v());
        if (mode_ == CPUMode::LONG_64BIT) {
            // In 64-bit mode, [RIP + disp32] relative to the next instruction pointer.
            // rip_ is currently right after disp32; any following immediate has imm_size bytes.
            addr = (rip_ + imm_size) + static_cast<uint64_t>(static_cast<int64_t>(disp));
        } else {
            addr = static_cast<uint64_t>(static_cast<int64_t>(disp));
        }
        return true;
    }

    uint64_t base_val = get_gpr(rm);
    int64_t disp = 0;
    if (mod == 1) {
        disp = static_cast<int8_t>(fetch8());
    } else if (mod == 2) {
        disp = static_cast<int32_t>(fetch32v());
    }
    addr = base_val + static_cast<uint64_t>(disp);
    return true;
}

// ---------------------------------------------------------------------------
// Fetch helpers
// ---------------------------------------------------------------------------
uint8_t X86CPUDecoder::fetch8() {
    uint8_t v = mem_->read8(rip_);
    rip_ += 1;
    return v;
}

uint16_t X86CPUDecoder::fetch16v() {
    uint16_t v = mem_->read16(rip_);
    rip_ += 2;
    return v;
}

uint32_t X86CPUDecoder::fetch32v() {
    uint32_t v = mem_->read32(rip_);
    rip_ += 4;
    return v;
}

uint64_t X86CPUDecoder::fetch_imm(uint32_t size) {
    switch (size) {
    case 1: return fetch8();
    case 2: return fetch16v();
    case 4: return fetch32v();
    default: {
        uint64_t lo = fetch32v();
        uint64_t hi = fetch32v();
        return lo | (hi << 32);
    }
    }
}

uint64_t X86CPUDecoder::fetch_sext_imm8(uint32_t size) {
    int8_t v = static_cast<int8_t>(fetch8());
    return static_cast<uint64_t>(static_cast<int64_t>(v)) & size_mask(size);
}

// x86 immediate encoding: 64-bit operand size still uses a 32-bit immediate
// (sign-extended). 16-bit ops use imm16, 8-bit ops imm8.
uint64_t X86CPUDecoder::fetch_imm_for_size(uint32_t size) {
    if (size == 8) {
        int32_t v = static_cast<int32_t>(fetch32v());
        return static_cast<uint64_t>(static_cast<int64_t>(v));
    }
    return fetch_imm(size);
}

uint32_t X86CPUDecoder::effective_operand_size() const {
    if (rex_present_ && (rex_ & 0x08)) return 8;  // REX.W
    if (prefix_66_) return 2;
    return (mode_ == CPUMode::LONG_64BIT) ? 4 : 2;
}

// ---------------------------------------------------------------------------
// ALU core
// ---------------------------------------------------------------------------
bool X86CPUDecoder::apply_alu(uint8_t aluop, uint64_t& dst, uint64_t src, uint32_t size) {
    const uint64_t mask = size_mask(size);
    uint64_t a = dst & mask;
    uint64_t b = src & mask;
    uint64_t r = 0;
    const bool carry_in = get_flag(kCF) ? 1 : 0;

    switch (aluop) {
    case 0:  // ADD
        r = a + b;
        update_flags_arith(r, a, b, size, false);
        break;
    case 1:  // OR
        r = a | b;
        update_flags_logic(r, size);
        break;
    case 2:  // ADC
        r = a + b + carry_in;
        update_flags_arith(r, a, b, size, false);
        set_flag(kCF, get_flag(kCF) || r < a || (carry_in && r == a));
        break;
    case 3:  // SBB
        r = a - b - carry_in;
        update_flags_arith(r, a, b, size, true);
        set_flag(kCF, a < b || (carry_in && a == b));
        break;
    case 4:  // AND
        r = a & b;
        update_flags_logic(r, size);
        break;
    case 5:  // SUB
        r = a - b;
        update_flags_arith(r, a, b, size, true);
        break;
    case 6:  // XOR
        r = a ^ b;
        update_flags_logic(r, size);
        break;
    case 7:  // CMP: flags only, no writeback
        r = a - b;
        update_flags_arith(r, a, b, size, true);
        return false;
    default:
        return false;
    }
    dst = r & mask;
    return true;
}

bool X86CPUDecoder::eval_condition(uint8_t cond) const {
    switch (cond & 0x0F) {
    case 0x0: return get_flag(kOF);
    case 0x1: return !get_flag(kOF);
    case 0x2: return get_flag(kCF);
    case 0x3: return !get_flag(kCF);
    case 0x4: return get_flag(kZF);
    case 0x5: return !get_flag(kZF);
    case 0x6: return get_flag(kCF) || get_flag(kZF);
    case 0x7: return !get_flag(kCF) && !get_flag(kZF);
    case 0x8: return get_flag(kSF);
    case 0x9: return !get_flag(kSF);
    case 0xA: return get_flag(kPF);
    case 0xB: return !get_flag(kPF);
    case 0xC: return get_flag(kSF) != get_flag(kOF);
    case 0xD: return get_flag(kSF) == get_flag(kOF);
    case 0xE: return get_flag(kZF) || (get_flag(kSF) != get_flag(kOF));
    default:  return !get_flag(kZF) && (get_flag(kSF) == get_flag(kOF));
    }
}

// ---------------------------------------------------------------------------
// Family 1: 0x00-0x3D — ALU <op> r/m, r  and  ALU <op> r, r/m
// ---------------------------------------------------------------------------
bool X86CPUDecoder::exec_alu_rm_reg(uint8_t opcode) {
    const uint8_t aluop = opcode >> 3;
    const bool direction = (opcode & 0x2) != 0;  // bit1: op r, r/m
    const uint32_t size = (opcode & 0x1) ? effective_operand_size() : 1;
    if (size == 1) rex_present_ = false;  // byte ops ignore REX.W semantics

    const uint8_t modrm = fetch8();
    if (opcode == 0x00 && modrm == 0x00) {
        // Uninitialized padding / zero trap: halt decoder and restore rip
        rip_ -= 2;
        return false;
    }
    uint32_t reg_field = 0, rm_field = 0;
    bool is_mem = false;
    uint64_t addr = 0;
    if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr)) return true;

    if (!direction) {
        // op r/m, reg
        uint64_t src = read_reg_sized(reg_field, size);
        if (is_mem) {
            uint64_t dst = read_mem_val(addr, size);
            if (apply_alu(aluop, dst, src, size)) write_mem_val(addr, dst, size);
        } else {
            uint64_t dst = read_reg_sized(rm_field, size);
            if (apply_alu(aluop, dst, src, size)) write_reg_sized(rm_field, dst, size);
        }
    } else {
        // op reg, r/m
        uint64_t src = is_mem ? read_mem_val(addr, size) : read_reg_sized(rm_field, size);
        uint64_t dst = read_reg_sized(reg_field, size);
        if (apply_alu(aluop, dst, src, size)) write_reg_sized(reg_field, dst, size);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Family 2: 0x88-0x8B — MOV r/m,r | r,r/m ; 0x8D LEA ; 0x86/0x87 XCHG
// ---------------------------------------------------------------------------
bool X86CPUDecoder::exec_mov_rm_reg(uint8_t opcode) {
    const uint32_t size = (opcode & 0x1) ? effective_operand_size() : 1;
    if (size == 1) rex_present_ = false;

    const uint8_t modrm = fetch8();
    uint32_t reg_field = 0, rm_field = 0;
    bool is_mem = false;
    uint64_t addr = 0;
    if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr)) return true;

    switch (opcode) {
    case 0x88:  // MOV r/m8, r8
    case 0x89:  // MOV r/m, r
        if (is_mem) {
            write_mem_val(addr, read_reg_sized(reg_field, size), size);
        } else {
            write_reg_sized(rm_field, read_reg_sized(reg_field, size), size);
        }
        break;
    case 0x8A:  // MOV r8, r/m8
    case 0x8B:  // MOV r, r/m
        write_reg_sized(reg_field, is_mem ? read_mem_val(addr, size) : read_reg_sized(rm_field, size), size);
        break;
    case 0x86:  // XCHG r/m8, r8
    case 0x87: {  // XCHG r/m, r
        uint64_t a = read_reg_sized(reg_field, size);
        uint64_t b = is_mem ? read_mem_val(addr, size) : read_reg_sized(rm_field, size);
        if (is_mem) {
            write_mem_val(addr, a, size);
        } else {
            write_reg_sized(rm_field, a, size);
        }
        write_reg_sized(reg_field, b, size);
        break;
    }
    case 0x8D: {  // LEA r, m
        if (is_mem) {
            write_reg_sized(reg_field, addr, size);
        }
        break;
    }
    default:
        break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Family 3: 0x80/0x81/0x83 — ALU r/m, imm ; also 0xC6/0xC7 MOV r/m, imm
// ---------------------------------------------------------------------------
bool X86CPUDecoder::exec_alu_immediate(uint8_t opcode) {
    // In 64-bit mode MOV r/m64, imm32 (REX.W + C7 /0) zero-sign-extends a
    // 32-bit immediate; decode the REX.W size explicitly.
    uint32_t size;
    if (opcode == 0xC7) {
        size = (rex_present_ && (rex_ & 0x08)) ? 8 : (prefix_66_ ? 2 : 4);
    } else if (opcode == 0x81 || opcode == 0x83) {
        size = effective_operand_size();
    } else {
        size = 1;
    }

    const uint8_t modrm = fetch8();
    const uint8_t aluop = (modrm >> 3) & 0x7;
    uint32_t reg_field = 0, rm_field = 0;
    bool is_mem = false;
    uint64_t addr = 0;

    uint32_t imm_size = 0;
    if (opcode == 0xC6 || opcode == 0x80 || opcode == 0x83) {
        imm_size = 1;
    } else if (opcode == 0xC7 || opcode == 0x81) {
        imm_size = prefix_66_ ? 2 : 4;
    }

    if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr, imm_size)) return true;

    if (opcode == 0xC6 || opcode == 0xC7) {  // MOV r/m, imm
        if (opcode == 0xC7 && size == 8) {
            // imm32 encoded, sign-extended to 64 bits.
            int32_t imm32 = static_cast<int32_t>(fetch32v());
            uint64_t imm = static_cast<uint64_t>(static_cast<int64_t>(imm32));
            if (is_mem) {
                write_mem_val(addr, imm, 8);
            } else {
                set_gpr(rm_field, imm);
            }
            return true;
        }
        uint64_t imm = fetch_imm(size);
        if (is_mem) {
            write_mem_val(addr, imm, size);
        } else {
            write_reg_sized(rm_field, imm, size);
        }
        return true;
    }

    uint64_t imm = (opcode == 0x83) ? fetch_sext_imm8(size) : fetch_imm_for_size(size);

    if (is_mem) {
        uint64_t dst = read_mem_val(addr, size);
        if (apply_alu(aluop, dst, imm, size)) write_mem_val(addr, dst, size);
    } else {
        uint64_t dst = read_reg_sized(rm_field, size);
        if (apply_alu(aluop, dst, imm, size)) write_reg_sized(rm_field, dst, size);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Family 4: 0xC0/0xC1/0xD0-0xD3 — shifts and rotates
// ---------------------------------------------------------------------------
bool X86CPUDecoder::exec_group2(uint8_t opcode) {
    const uint32_t size = (opcode & 0x1) ? effective_operand_size() : 1;
    if (size == 1) rex_present_ = false;

    const uint8_t modrm = fetch8();
    uint32_t reg_field = 0, rm_field = 0;
    bool is_mem = false;
    uint64_t addr = 0;
    const uint32_t imm_size = (opcode == 0xC0 || opcode == 0xC1) ? 1 : 0;
    if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr, imm_size)) return true;

    uint32_t count = 0;
    switch (opcode) {
    case 0xC0: case 0xC1: count = fetch8() & 0x1F; break;
    case 0xD0: case 0xD1: count = 1; break;
    default: count = static_cast<uint32_t>(get_gpr(1) & 0x1F); break;  // CL
    }

    const uint64_t mask = size_mask(size);
    const uint32_t bits = size * 8;
    uint64_t val = is_mem ? read_mem_val(addr, size) : read_reg_sized(rm_field, size);
    uint64_t result = val;

    if (count != 0) {
        switch (reg_field & 0x7) {
        case 0: {  // ROL
            uint32_t c = count % bits;
            if (c == 0) {
                set_flag(kCF, val & 1);
            } else {
                result = ((val << c) | (val >> (bits - c))) & mask;
                set_flag(kCF, result & 1);
            }
            break;
        }
        case 1: {  // ROR
            uint32_t c = count % bits;
            if (c == 0) {
                set_flag(kCF, (val >> (bits - 1)) & 1);
            } else {
                result = ((val >> c) | (val << (bits - c))) & mask;
                set_flag(kCF, (result >> (bits - 1)) & 1);
            }
            break;
        }
        case 2: {  // RCL (rotate through carry)
            uint32_t c = count % (bits + 1);
            uint64_t r = val & mask;
            for (uint32_t i = 0; i < c; ++i) {
                bool top = (r >> (bits - 1)) & 1;
                r = ((r << 1) & mask) | (get_flag(kCF) ? 1 : 0);
                set_flag(kCF, top);
            }
            result = r;
            break;
        }
        case 3: {  // RCR
            uint32_t c = count % (bits + 1);
            uint64_t r = val & mask;
            for (uint32_t i = 0; i < c; ++i) {
                bool bottom = r & 1;
                r = (r >> 1) | (get_flag(kCF) ? (1ULL << (bits - 1)) : 0);
                set_flag(kCF, bottom);
            }
            result = r;
            break;
        }
        case 4: case 6: {  // SHL / SAL
            if (count >= bits) {
                set_flag(kCF, count == bits ? (val & 1) != 0 : false);
                result = 0;
            } else {
                result = (val << count) & mask;
                set_flag(kCF, (val >> (bits - count)) & 1);
            }
            update_flags_shift(result, size, count);
            break;
        }
        case 5: {  // SHR
            result = val >> count;
            set_flag(kCF, (val >> (count - 1)) & 1);
            update_flags_shift(result, size, count);
            break;
        }
        case 7: {  // SAR
            int64_t sext;
            if (bits == 64) {
                sext = static_cast<int64_t>(val);
            } else {
                sext = static_cast<int64_t>(static_cast<uint64_t>(val << (64 - bits)) >> (64 - bits));
            }
            result = (static_cast<uint64_t>(sext >> count)) & mask;
            set_flag(kCF, (static_cast<uint64_t>(sext >> (count - 1))) & 1);
            update_flags_shift(result, size, count);
            break;
        }
        }
    }

    if (is_mem) {
        write_mem_val(addr, result, size);
    } else {
        write_reg_sized(rm_field, result, size);
    }
    return true;
}

// ---------------------------------------------------------------------------
// Family 5: 0xF6/0xF7 — TEST/NOT/NEG/MUL/IMUL/DIV/IDIV
// ---------------------------------------------------------------------------
bool X86CPUDecoder::exec_group3(uint8_t opcode) {
    const uint32_t size = (opcode & 0x1) ? effective_operand_size() : 1;
    if (size == 1) rex_present_ = false;

    const uint8_t modrm = fetch8();
    const uint8_t sub = (modrm >> 3) & 0x7;
    uint32_t reg_field = 0, rm_field = 0;
    bool is_mem = false;
    uint64_t addr = 0;
    const uint32_t imm_size = (sub <= 1) ? ((opcode == 0xF6) ? 1 : (prefix_66_ ? 2 : 4)) : 0;
    if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr, imm_size)) return true;

    const uint64_t mask = size_mask(size);
    const uint32_t bits = size * 8;
    uint64_t val = is_mem ? read_mem_val(addr, size) : read_reg_sized(rm_field, size);

    auto writeback = [&](uint64_t r) {
        if (is_mem) write_mem_val(addr, r, size);
        else write_reg_sized(rm_field, r, size);
    };

    switch (sub) {
    case 0: case 1: {  // TEST r/m, imm
        uint64_t imm = fetch_imm_for_size(size);
        update_flags_logic(val & (imm & mask), size);
        break;
    }
    case 2:  // NOT (no flags)
        writeback((~val) & mask);
        break;
    case 3: {  // NEG
        uint64_t r = (0 - val) & mask;
        update_flags_arith(r, 0, val, size, true);
        writeback(r);
        break;
    }
    case 4: {  // MUL (unsigned)
        if (size == 1) {
            uint64_t r = (get_gpr(0) & 0xFF) * (val & 0xFF);
            write_reg_sized(0, r & 0xFF, 1);
            write_reg_sized(4, (r >> 8) & 0xFF, 1);
            set_flag(kCF, (r >> 8) != 0);
            set_flag(kOF, (r >> 8) != 0);
        } else {
            unsigned __int128 wide = static_cast<unsigned __int128>(read_reg_sized(0, size)) * (val & mask);
            uint64_t lo = static_cast<uint64_t>(wide);
            uint64_t hi = static_cast<uint64_t>(wide >> 64);
            write_reg_sized(0, lo, size);
            if (size == 2) write_reg_sized(2, (lo >> 16) & 0xFFFF, 2);
            else if (size == 4) write_reg_sized(2, (lo >> 32) & 0xFFFFFFFFULL, 4);
            else write_reg_sized(2, hi, 8);
            bool carry = (size == 2) ? ((lo >> 16) != 0)
                       : (size == 4) ? ((lo >> 32) != 0) : (hi != 0);
            set_flag(kCF, carry);
            set_flag(kOF, carry);
        }
        break;
    }
    case 5: {  // IMUL (one-operand, signed)
        __int128 sa, sb;
        if (size == 1) {
            sa = static_cast<int8_t>(get_gpr(0) & 0xFF);
            sb = static_cast<int8_t>(val & 0xFF);
        } else if (size == 2) {
            sa = static_cast<int16_t>(read_reg_sized(0, 2));
            sb = static_cast<int16_t>(val & 0xFFFF);
        } else if (size == 4) {
            sa = static_cast<int32_t>(read_reg_sized(0, 4));
            sb = static_cast<int32_t>(val & 0xFFFFFFFFULL);
        } else {
            sa = static_cast<int64_t>(read_reg_sized(0, 8));
            sb = static_cast<int64_t>(val);
        }
        __int128 p = sa * sb;
        uint64_t lo = static_cast<uint64_t>(p);
        write_reg_sized(0, lo, size);
        if (size == 1) {
            uint16_t prod16 = static_cast<uint16_t>(lo & 0xFFFF);
            write_reg_sized(4, (prod16 >> 8) & 0xFF, 1);
        } else if (size == 2) write_reg_sized(2, (lo >> 16) & 0xFFFF, 2);
        else if (size == 4) write_reg_sized(2, (lo >> 32) & 0xFFFFFFFFULL, 4);
        else write_reg_sized(2, static_cast<uint64_t>(static_cast<unsigned __int128>(p) >> 64), 8);

        int64_t sext;
        if (bits == 64) sext = static_cast<int64_t>(lo);
        else sext = static_cast<int64_t>(static_cast<uint64_t>(lo << (64 - bits)) >> (64 - bits));
        bool ovf = static_cast<__int128>(sext) != p;
        set_flag(kCF, ovf);
        set_flag(kOF, ovf);
        break;
    }
    case 6: {  // DIV (unsigned); divide error halts the vCPU
        uint64_t divisor = val & mask;
        if (divisor == 0) return false;
        if (size == 1) {
            uint16_t dividend = static_cast<uint16_t>(get_gpr(0) & 0xFFFF);
            uint16_t q = static_cast<uint16_t>(dividend / divisor);
            uint16_t r = static_cast<uint16_t>(dividend % divisor);
            if (q > 0xFF) return false;
            write_reg_sized(0, (static_cast<uint64_t>(r) << 8) | (q & 0xFF), 2);
        } else if (size == 2) {
            uint32_t dividend = (static_cast<uint32_t>(read_reg_sized(2, 2)) << 16) |
                                 static_cast<uint32_t>(read_reg_sized(0, 2));
            uint32_t d = static_cast<uint32_t>(divisor);
            uint32_t q = dividend / d;
            if (q > 0xFFFF) return false;
            write_reg_sized(0, q & 0xFFFF, 2);
            write_reg_sized(2, (dividend % d) & 0xFFFF, 2);
        } else if (size == 4) {
            uint64_t dividend = (static_cast<uint64_t>(read_reg_sized(2, 4)) << 32) |
                                 read_reg_sized(0, 4);
            uint64_t q = dividend / divisor;
            if (q > 0xFFFFFFFFULL) return false;
            write_reg_sized(0, q & 0xFFFFFFFFULL, 4);
            write_reg_sized(2, (dividend % divisor) & 0xFFFFFFFFULL, 4);
        } else {
            unsigned __int128 dividend = (static_cast<unsigned __int128>(read_reg_sized(2, 8)) << 64) |
                                          read_reg_sized(0, 8);
            write_reg_sized(0, static_cast<uint64_t>(dividend / divisor), 8);
            write_reg_sized(2, static_cast<uint64_t>(dividend % divisor), 8);
        }
        break;
    }
    case 7: {  // IDIV (signed); divide error halts the vCPU
        __int128 dividend, divisor_s;
        if (size == 1) {
            dividend = static_cast<int16_t>(get_gpr(0) & 0xFFFF);
            divisor_s = static_cast<int8_t>(val & 0xFF);
        } else if (size == 2) {
            dividend = static_cast<int32_t>((static_cast<uint32_t>(read_reg_sized(2, 2)) << 16) |
                                             static_cast<uint32_t>(read_reg_sized(0, 2)));
            divisor_s = static_cast<int16_t>(val & 0xFFFF);
        } else if (size == 4) {
            dividend = (static_cast<int64_t>(read_reg_sized(2, 4)) << 32) | read_reg_sized(0, 4);
            divisor_s = static_cast<int32_t>(val & 0xFFFFFFFFULL);
        } else {
            dividend = (static_cast<__int128>(read_reg_sized(2, 8)) << 64) | read_reg_sized(0, 8);
            divisor_s = static_cast<int64_t>(val);
        }
        if (divisor_s == 0) return false;
        __int128 q = dividend / divisor_s;
        __int128 r = dividend % divisor_s;
        __int128 qmin = -(static_cast<__int128>(1) << (bits - 1));
        __int128 qmax = (static_cast<__int128>(1) << (bits - 1)) - 1;
        if (q < qmin || q > qmax) return false;
        if (size == 1) {
            write_reg_sized(0, static_cast<uint64_t>(q) & 0xFF, 1);
            write_reg_sized(4, static_cast<uint64_t>(r) & 0xFF, 1);
        } else {
            write_reg_sized(0, static_cast<uint64_t>(q) & mask, size);
            write_reg_sized(2, static_cast<uint64_t>(r) & mask, size);
        }
        break;
    }
    default:
        break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 0x40-0x4F — INC/DEC r (16-bit forms handled as INC/DEC without REX)
// ---------------------------------------------------------------------------
bool X86CPUDecoder::exec_incdec_group(uint8_t opcode) {
    const uint32_t size = effective_operand_size();
    const uint32_t idx = (opcode & 0x7) | ((rex_present_ && (rex_ & 0x01)) ? 8 : 0);
    uint64_t val = read_reg_sized(idx, size);
    uint64_t r;
    if (opcode & 0x8) {
        r = (val - 1) & size_mask(size);
        update_flags_incdec(r, val, size, false);
    } else {
        r = (val + 1) & size_mask(size);
        update_flags_incdec(r, val, size, true);
    }
    write_reg_sized(idx, r, size);
    return true;
}

// ---------------------------------------------------------------------------
// Stack operations: 0x50-0x5F, 0x68, 0x6A, 0x8F, 0x9C, 0x9D
// ---------------------------------------------------------------------------
bool X86CPUDecoder::exec_push_pop(uint8_t opcode) {
    const uint32_t size = effective_operand_size();

    auto push = [&](uint64_t value) {
        uint64_t rsp = get_gpr(4) - size;
        set_gpr(4, rsp);
        write_mem_val(rsp, value & size_mask(size), size);
    };
    auto pop = [&]() -> uint64_t {
        uint64_t rsp = get_gpr(4);
        uint64_t v = read_mem_val(rsp, size);
        set_gpr(4, rsp + size);
        return v;
    };

    if (opcode >= 0x50 && opcode <= 0x57) {
        push(read_reg_sized((opcode & 0x7) | ((rex_present_ && (rex_ & 0x01)) ? 8 : 0), size));
        return true;
    }
    if (opcode >= 0x58 && opcode <= 0x5F) {
        write_reg_sized((opcode & 0x7) | ((rex_present_ && (rex_ & 0x01)) ? 8 : 0), pop(), size);
        return true;
    }

    switch (opcode) {
    case 0x68: push(fetch_imm(size)); break;
    case 0x6A: push(fetch_sext_imm8(size)); break;
    case 0x8F: {
        uint64_t v = pop();
        const uint8_t modrm = fetch8();
        uint32_t reg_field = 0, rm_field = 0;
        bool is_mem = false;
        uint64_t addr = 0;
        if (decode_modrm(modrm, reg_field, rm_field, is_mem, addr)) {
            if (is_mem) write_mem_val(addr, v, size);
            else write_reg_sized(rm_field, v, size);
        }
        break;
    }
    case 0x9C: push(rflags_); break;  // PUSHF
    case 0x9D:                        // POPF
        rflags_ = (rflags_ & ~0xFFFFFFFFULL) | (pop() & 0x003F7FD7ULL) | 0x02;
        break;
    default:
        break;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 0x0F-prefixed two-byte opcodes
// ---------------------------------------------------------------------------
bool X86CPUDecoder::exec_two_byte(uint8_t opcode) {
    const uint32_t size = effective_operand_size();

    if (opcode == 0x05) return false;              // SYSCALL: treat as halt
    if (opcode == 0x0B) return false;              // UD2

    if (opcode == 0x1F) {                          // multi-byte NOP
        const uint8_t modrm = fetch8();
        uint32_t reg_field = 0, rm_field = 0;
        bool is_mem = false;
        uint64_t addr = 0;
        decode_modrm(modrm, reg_field, rm_field, is_mem, addr);
        return true;
    }

    if (opcode == 0x0F || opcode == 0x18 || opcode == 0x19 || opcode == 0x1E ||
        opcode == 0x1F + 0x20) {
        // Prefetch hints / 3DNow nop-ish: treat 0F 18-1F as hints.
        if (opcode >= 0x18 && opcode <= 0x1F) {
            const uint8_t modrm = fetch8();
            uint32_t reg_field = 0, rm_field = 0;
            bool is_mem = false;
            uint64_t addr = 0;
            decode_modrm(modrm, reg_field, rm_field, is_mem, addr);
            return true;
        }
    }

    if (opcode == 0xAF) {                          // IMUL r, r/m
        const uint8_t modrm = fetch8();
        uint32_t reg_field = 0, rm_field = 0;
        bool is_mem = false;
        uint64_t addr = 0;
        if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr)) return true;
        uint64_t b = is_mem ? read_mem_val(addr, size) : read_reg_sized(rm_field, size);
        uint64_t a = read_reg_sized(reg_field, size);
        const uint64_t mask = size_mask(size);
        if (size == 2) {
            int32_t p = static_cast<int32_t>(static_cast<int16_t>(a & 0xFFFF)) *
                        static_cast<int32_t>(static_cast<int16_t>(b & 0xFFFF));
            write_reg_sized(reg_field, static_cast<uint64_t>(p) & 0xFFFF, 2);
            set_flag(kCF, (p < -32768) || (p > 32767));
        } else if (size == 4) {
            int64_t p = static_cast<int64_t>(static_cast<int32_t>(a & 0xFFFFFFFFULL)) *
                        static_cast<int64_t>(static_cast<int32_t>(b & 0xFFFFFFFFULL));
            write_reg_sized(reg_field, static_cast<uint64_t>(p) & 0xFFFFFFFFULL, 4);
            set_flag(kCF, (p < -2147483648LL) || (p > 2147483647LL));
        } else {
            __int128 p = static_cast<__int128>(static_cast<int64_t>(a)) *
                         static_cast<__int128>(static_cast<int64_t>(b));
            uint64_t lo = static_cast<uint64_t>(p);
            write_reg_sized(reg_field, lo, 8);
            int64_t sext = static_cast<int64_t>(lo);
            set_flag(kCF, static_cast<__int128>(sext) != p);
        }
        set_flag(kOF, get_flag(kCF));
        return true;
    }

    if (opcode == 0xB6 || opcode == 0xB7 || opcode == 0xBE || opcode == 0xBF) {
        // MOVZX / MOVSX r, r/m8|16
        const uint32_t src_size = (opcode == 0xB6 || opcode == 0xBE) ? 1 : 2;
        const uint8_t modrm = fetch8();
        uint32_t reg_field = 0, rm_field = 0;
        bool is_mem = false;
        uint64_t addr = 0;
        if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr)) return true;
        uint64_t v = is_mem ? read_mem_val(addr, src_size) : read_reg_sized(rm_field, src_size);
        if (opcode == 0xBE || opcode == 0xBF) {
            int64_t sval = (src_size == 1) ? static_cast<int8_t>(v & 0xFF)
                                           : static_cast<int16_t>(v & 0xFFFF);
            write_reg_sized(reg_field, static_cast<uint64_t>(sval), size);
        } else {
            write_reg_sized(reg_field, v & size_mask(src_size), size);
        }
        return true;
    }

    if (opcode == 0x05) return false;

    // Conditional family: 0F 4x CMOV, 0F 8x Jcc rel32, 0F 9x SETcc
    const uint8_t family = opcode & 0xF0;
    const uint8_t cond = opcode & 0x0F;

    if (family == 0x80) {                          // Jcc rel32
        int32_t rel = static_cast<int32_t>(fetch32v());
        if (eval_condition(cond)) {
            rip_ += static_cast<uint64_t>(rel);
        }
        return true;
    }
    if (family == 0x90) {                          // SETcc r/m8
        const uint8_t modrm = fetch8();
        uint32_t reg_field = 0, rm_field = 0;
        bool is_mem = false;
        uint64_t addr = 0;
        if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr)) return true;
        const uint8_t v = eval_condition(cond) ? 1 : 0;
        if (is_mem) write_mem_val(addr, v, 1);
        else write_reg_sized(rm_field, v, 1);
        return true;
    }
    if (family == 0x40) {                          // CMOVcc r, r/m
        const uint8_t modrm = fetch8();
        uint32_t reg_field = 0, rm_field = 0;
        bool is_mem = false;
        uint64_t addr = 0;
        if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr)) return true;
        if (eval_condition(cond)) {
            uint64_t v = is_mem ? read_mem_val(addr, size) : read_reg_sized(rm_field, size);
            write_reg_sized(reg_field, v, size);
        }
        return true;
    }

    switch (opcode) {
    case 0x05: return false;
    case 0x22: case 0x23: case 0x20: case 0x21:    // MOV CR/DR (accepted, ignored)
        fetch8();
        return true;
    case 0xA2: return true;                        // CPUID: leave registers untouched
    case 0x31: {                                   // RDTSC: vCPU cycle counter proxy
        static uint64_t fake_tsc = 0;
        fake_tsc += 100;
        set_gpr(0, fake_tsc);
        set_gpr(2, 0);
        return true;
    }
    case 0xA3: {                                   // BT r/m, r
        const uint8_t modrm = fetch8();
        uint32_t reg_field = 0, rm_field = 0;
        bool is_mem = false;
        uint64_t addr = 0;
        if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr)) return true;
        uint64_t base = is_mem ? addr : get_gpr(rm_field);
        uint64_t bit = get_gpr(reg_field);
        uint64_t byte_addr = base + (bit >> 3);
        uint8_t bit_in = static_cast<uint8_t>(bit & 7);
        uint8_t byte_val = mem_->read8(byte_addr);
        set_flag(kCF, (byte_val >> bit_in) & 1);
        return true;
    }
    case 0xAB: case 0xB3: case 0xBB: case 0xB1: case 0xB0: {  // BTS/BTR/BTC/BTS
        const uint8_t modrm = fetch8();
        uint32_t reg_field = 0, rm_field = 0;
        bool is_mem = false;
        uint64_t addr = 0;
        if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr)) return true;
        uint64_t base = is_mem ? addr : get_gpr(rm_field);
        uint64_t bit = get_gpr(reg_field);
        uint64_t byte_addr = base + (bit >> 3);
        uint8_t bit_in = static_cast<uint8_t>(bit & 7);
        uint8_t byte_val = mem_->read8(byte_addr);
        set_flag(kCF, (byte_val >> bit_in) & 1);
        uint8_t new_val = byte_val;
        switch (opcode) {
        case 0xAB: case 0xB3: new_val |= (1 << bit_in); break;   // BTS (0xAB) / LOCK BTS alias
        case 0xBB: new_val &= ~(1 << bit_in); break;             // BTR
        case 0xB1: new_val ^= (1 << bit_in); break;              // XOR alias -> BTC-ish
        case 0xB0: new_val ^= (1 << bit_in); break;              // BTC alias
        default: break;
        }
        mem_->write8(byte_addr, new_val);
        return true;
    }
    case 0xBC: case 0xBD: {                        // BSF / BSR
        const uint8_t modrm = fetch8();
        uint32_t reg_field = 0, rm_field = 0;
        bool is_mem = false;
        uint64_t addr = 0;
        if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr)) return true;
        uint64_t v = is_mem ? read_mem_val(addr, size) : read_reg_sized(rm_field, size);
        if (v == 0) {
            set_flag(kZF, true);
        } else {
            set_flag(kZF, false);
            uint64_t result = (opcode == 0xBC) ? static_cast<uint64_t>(__builtin_ctzll(v))
                                               : static_cast<uint64_t>(63 - __builtin_clzll(v));
            write_reg_sized(reg_field, result, size);
        }
        return true;
    }
    default:
        break;
    }

    // Unknown two-byte opcode: treat as halt signal.
    return false;
}

// ---------------------------------------------------------------------------
// Main dispatch
// ---------------------------------------------------------------------------
bool X86CPUDecoder::step() {
    if (!mem_) return false;

    rex_present_ = false;
    rex_ = 0;
    prefix_66_ = false;

    uint8_t opcode = fetch8();

    // Prefix loop
    while (true) {
        if (opcode >= 0x40 && opcode <= 0x4F) {
            // REX prefix in 64-bit mode, INC/DEC in real/protected mode.
            if (mode_ == CPUMode::LONG_64BIT) {
                rex_present_ = true;
                rex_ = opcode & 0x0F;
                opcode = fetch8();
                continue;
            }
            break;  // handled as INC/DEC below
        }
        if (opcode == 0x66) {
            prefix_66_ = true;
            opcode = fetch8();
            continue;
        }
        if (opcode == 0x26 || opcode == 0x2E || opcode == 0x36 || opcode == 0x3E ||
            opcode == 0x64 || opcode == 0x65 ||  // segment overrides (flat model)
            opcode == 0x67 ||                    // address-size override
            opcode == 0xF2 || opcode == 0xF3) {  // rep prefixes (no string ops yet)
            opcode = fetch8();
            continue;
        }
        if (opcode == 0xF0) {                    // LOCK: single-core model, ignore
            opcode = fetch8();
            continue;
        }
        break;
    }

    switch (opcode) {
    case 0x00: case 0x01: case 0x02: case 0x03:
    case 0x08: case 0x09: case 0x0A: case 0x0B:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x38: case 0x39: case 0x3A: case 0x3B:
        return exec_alu_rm_reg(opcode);

    case 0x04: case 0x05: case 0x0C: case 0x0D:    // ALU eax, imm
    case 0x14: case 0x15: case 0x1C: case 0x1D:
    case 0x24: case 0x25: case 0x2C: case 0x2D:
    case 0x34: case 0x35: case 0x3C: case 0x3D: {
        const uint8_t aluop = opcode >> 3;
        // Low bit selects acc,immz (odd opcodes) vs AL,imm8 (even opcodes).
        const uint32_t size = (opcode & 0x1) ? effective_operand_size() : 1;
        uint64_t imm = (opcode & 0x1) ? fetch_imm_for_size(size) : fetch_sext_imm8(size);
        uint64_t acc = read_reg_sized(0, size);
        if (apply_alu(aluop, acc, imm, size)) write_reg_sized(0, acc, size);
        return true;
    }

    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
    case 0x68: case 0x6A: case 0x8F:
    case 0x9C: case 0x9D:
        return exec_push_pop(opcode);

    case 0x63: {                                   // MOVSXD r64, r/m32
        const uint8_t modrm = fetch8();
        uint32_t reg_field = 0, rm_field = 0;
        bool is_mem = false;
        uint64_t addr = 0;
        if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr)) return true;
        uint32_t src_size = prefix_66_ ? 2 : 4;
        uint64_t v = is_mem ? read_mem_val(addr, src_size) : read_reg_sized(rm_field, src_size);
        int64_t sval = (src_size == 2) ? static_cast<int16_t>(v) : static_cast<int32_t>(v);
        uint32_t dst_size = 8;
        if (prefix_66_) dst_size = 2;
        else if (mode_ != CPUMode::LONG_64BIT && !(rex_present_ && (rex_ & 0x08))) dst_size = 4;
        write_reg_sized(reg_field, static_cast<uint64_t>(sval), dst_size);
        return true;
    }

    case 0x70: case 0x71: case 0x72: case 0x73:    // Jcc rel8
    case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t rel = static_cast<int8_t>(fetch8());
        if (eval_condition(opcode & 0x0F)) {
            rip_ += static_cast<uint64_t>(rel);
        }
        return true;
    }

    case 0x80: case 0x81: case 0x83:
        return exec_alu_immediate(opcode);

    case 0x84: case 0x85: {                        // TEST r/m, r
        const uint32_t size = (opcode & 0x1) ? effective_operand_size() : 1;
        if (size == 1) rex_present_ = false;
        const uint8_t modrm = fetch8();
        uint32_t reg_field = 0, rm_field = 0;
        bool is_mem = false;
        uint64_t addr = 0;
        if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr)) return true;
        uint64_t a = read_reg_sized(reg_field, size);
        uint64_t b = is_mem ? read_mem_val(addr, size) : read_reg_sized(rm_field, size);
        update_flags_logic(a & b, size);
        return true;
    }
    case 0xA8: case 0xA9: {                        // TEST al, imm
        const uint32_t size = (opcode & 0x1) ? effective_operand_size() : 1;
        uint64_t imm = fetch_imm_for_size(size);
        update_flags_logic(read_reg_sized(0, size) & (imm & size_mask(size)), size);
        return true;
    }

    case 0x86: case 0x87: case 0x88: case 0x89:
    case 0x8A: case 0x8B: case 0x8D:
        return exec_mov_rm_reg(opcode);

    case 0x8C: case 0x8E: {                        // MOV r/m, Sreg (accepted)
        const uint8_t modrm = fetch8();
        uint32_t reg_field = 0, rm_field = 0;
        bool is_mem = false;
        uint64_t addr = 0;
        decode_modrm(modrm, reg_field, rm_field, is_mem, addr);
        return true;
    }

    case 0xB8: case 0xB9: case 0xBA: case 0xBB:    // MOV r, imm
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        const uint32_t size = effective_operand_size();
        const uint32_t idx = (opcode & 0x7) | ((rex_present_ && (rex_ & 0x01)) ? 8 : 0);
        uint64_t imm;
        if (rex_present_ && (rex_ & 0x08)) {
            // REX.W + B8+: full 64-bit immediate.
            uint64_t lo = fetch32v();
            uint64_t hi = fetch32v();
            imm = lo | (hi << 32);
            set_gpr(idx, imm);
        } else {
            imm = fetch_imm(size);
            write_reg_sized(idx, imm, size);
        }
        return true;
    }

    case 0x90:                                     // NOP
        return true;
    case 0x91: case 0x92: case 0x93:               // XCHG eax, r
    case 0x94: case 0x95: case 0x96: case 0x97: {
        const uint32_t size = effective_operand_size();
        const uint32_t idx = (opcode & 0x7) | ((rex_present_ && (rex_ & 0x01)) ? 8 : 0);
        uint64_t a = read_reg_sized(0, size);
        uint64_t b = read_reg_sized(idx, size);
        write_reg_sized(0, b, size);
        write_reg_sized(idx, a, size);
        return true;
    }

    case 0x98: {                                   // CBW/CWDE/CDQE
        const uint32_t size = effective_operand_size();
        if (size == 2) {
            int8_t v = static_cast<int8_t>(get_gpr(0) & 0xFF);
            write_reg_sized(0, static_cast<uint64_t>(static_cast<int64_t>(v)) & 0xFFFF, 2);
        } else if (size == 4) {
            int16_t v = static_cast<int16_t>(get_gpr(0) & 0xFFFF);
            write_reg_sized(0, static_cast<uint64_t>(static_cast<int64_t>(v)) & 0xFFFFFFFFULL, 4);
        } else {
            int32_t v = static_cast<int32_t>(get_gpr(0) & 0xFFFFFFFFULL);
            set_gpr(0, static_cast<uint64_t>(static_cast<int64_t>(v)));
        }
        return true;
    }
    case 0x99: {                                   // CWD/CDQ/CQO
        const uint32_t size = effective_operand_size();
        uint64_t sign = (read_reg_sized(0, size) >> (size * 8 - 1)) & 1;
        uint64_t fill = sign ? size_mask(size) : 0;
        if (size == 2) write_reg_sized(2, fill & 0xFFFF, 2);
        else if (size == 4) write_reg_sized(2, fill & 0xFFFFFFFFULL, 4);
        else set_gpr(2, fill);
        return true;
    }

    case 0xE8: {                                   // CALL rel32
        int32_t rel = static_cast<int32_t>(fetch32v());
        uint32_t size = effective_operand_size();
        uint64_t ret_addr = rip_;
        uint64_t rsp = get_gpr(4) - size;
        set_gpr(4, rsp);
        write_mem_val(rsp, ret_addr & size_mask(size), size);
        rip_ += static_cast<uint64_t>(rel);
        return true;
    }
    case 0xC3: {                                   // RET
        uint32_t size = effective_operand_size();
        uint64_t rsp = get_gpr(4);
        rip_ = read_mem_val(rsp, size);
        set_gpr(4, rsp + size);
        return true;
    }
    case 0xC2: {                                   // RET imm16
        uint16_t imm = fetch16v();
        uint32_t size = effective_operand_size();
        uint64_t rsp = get_gpr(4);
        rip_ = read_mem_val(rsp, size);
        set_gpr(4, rsp + size + imm);
        return true;
    }

    case 0xE9: {                                   // JMP rel32
        int32_t rel = static_cast<int32_t>(fetch32v());
        rip_ += static_cast<uint64_t>(rel);
        return true;
    }
    case 0xEB: {                                   // JMP rel8
        int8_t rel = static_cast<int8_t>(fetch8());
        rip_ += static_cast<uint64_t>(rel);
        return true;
    }

    case 0xE0: case 0xE1: case 0xE2: {             // LOOPNZ / LOOPZ / LOOP
        int8_t rel = static_cast<int8_t>(fetch8());
        uint32_t size = effective_operand_size();
        uint64_t cx = (read_reg_sized(1, size) - 1) & size_mask(size);
        write_reg_sized(1, cx, size);
        bool loop = (cx != 0);
        if (opcode == 0xE0) loop = loop && !get_flag(kZF);
        if (opcode == 0xE1) loop = loop && get_flag(kZF);
        if (loop) rip_ += static_cast<uint64_t>(rel);
        return true;
    }

    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3:
        return exec_group2(opcode);

    case 0xF6: case 0xF7:
        return exec_group3(opcode);

    case 0xC6: case 0xC7:
        return exec_alu_immediate(opcode);

    case 0xFE: case 0xFF: {                        // INC/DEC r/m + indirect calls
        const uint32_t size = (opcode & 0x1) ? effective_operand_size() : 1;
        if (size == 1) rex_present_ = false;
        const uint8_t modrm = fetch8();
        const uint8_t sub = (modrm >> 3) & 0x7;
        uint32_t reg_field = 0, rm_field = 0;
        bool is_mem = false;
        uint64_t addr = 0;
        if (!decode_modrm(modrm, reg_field, rm_field, is_mem, addr)) return true;

        switch (sub) {
        case 0: {  // INC r/m
            uint64_t v = is_mem ? read_mem_val(addr, size) : read_reg_sized(rm_field, size);
            uint64_t r = (v + 1) & size_mask(size);
            update_flags_incdec(r, v, size, true);
            if (is_mem) write_mem_val(addr, r, size);
            else write_reg_sized(rm_field, r, size);
            return true;
        }
        case 1: {  // DEC r/m
            uint64_t v = is_mem ? read_mem_val(addr, size) : read_reg_sized(rm_field, size);
            uint64_t r = (v - 1) & size_mask(size);
            update_flags_incdec(r, v, size, false);
            if (is_mem) write_mem_val(addr, r, size);
            else write_reg_sized(rm_field, r, size);
            return true;
        }
        case 2: {  // CALL r/m
            uint64_t target = is_mem ? read_mem_val(addr, size) : get_gpr(rm_field);
            uint64_t ret_addr = rip_;
            uint64_t rsp = get_gpr(4) - size;
            set_gpr(4, rsp);
            write_mem_val(rsp, ret_addr & size_mask(size), size);
            rip_ = target;
            return true;
        }
        case 4: {  // JMP r/m
            uint64_t target = is_mem ? read_mem_val(addr, size) : get_gpr(rm_field);
            rip_ = target;
            return true;
        }
        case 6: {  // PUSH r/m
            uint64_t v = is_mem ? read_mem_val(addr, size) : read_reg_sized(rm_field, size);
            uint64_t rsp = get_gpr(4) - size;
            set_gpr(4, rsp);
            write_mem_val(rsp, v & size_mask(size), size);
            return true;
        }
        default:
            return false;  // unsupported group-5 sub-op
        }
    }

    case 0xF8: set_flag(kCF, false); return true;  // CLC
    case 0xF9: set_flag(kCF, true); return true;   // STC
    case 0xF5: set_flag(kCF, !get_flag(kCF)); return true;  // CMC
    case 0xFC: set_flag(10, false); return true;   // CLD (DF = bit 10)
    case 0xFD: set_flag(10, true); return true;    // STD
    case 0xFA: case 0xFB: return true;             // CLI/STI (no interrupt model)

    case 0xF4:                                     // HLT
        return false;
    case 0xCC:                                     // INT3
        return false;

    case 0x0F:
        return exec_two_byte(fetch8());

    default:
        if (opcode >= 0x40 && opcode <= 0x4F) {
            return exec_incdec_group(opcode);
        }
        // Unknown opcode: halt (visible to tests and the run loop).
        return false;
    }
}

}  // namespace lime
