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

// ---------------------------------------------------------------------------
// X86CPUDecoder — software x86_64 target decoder.
//
// Covers the integer baseline needed to boot simple kernels:
//   * prefixes: REX (W/R/X/B), 0x66 operand size, segment/rep (ignored)
//   * data movement: MOV r/m,r | r,r/m | r,imm (8/16/32/64), MOVZX, MOVSX,
//     LEA, XCHG
//   * ALU: ADD/OR/ADC/SBB/AND/SUB/XOR/CMP in register, memory and immediate
//     forms (0x00-0x3D, 0x80/0x81/0x83 groups), INC/DEC, NEG, NOT
//   * shifts/rotates: SHL/SHR/SAR/ROL/ROR/RCL/RCR by 1, imm8 and CL
//   * control flow: JMP rel8/rel32, Jcc rel8 + 0F 8x rel32, CALL/RET,
//     LOOP/LOOPE/LOOPNE, NOP (0x90, 0F 1F)
//   * stack: PUSH/POP r16/64, PUSH imm8/imm32, POP r/m
//   * flags: full CF/PF/ZF/SF/OF maintenance, CLC/STC/CMC/CLD/STD/CLI/STI
//   * system: HLT, INT3
// Memory operands use the flat 64-bit linear model (no segmentation).
// ---------------------------------------------------------------------------
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

    uint64_t get_rflags() const { return rflags_; }

    CPUMode mode() const;
    void set_mode(CPUMode m);

private:
    // Flag bit positions in rflags_.
    static constexpr int kCF = 0;
    static constexpr int kPF = 2;
    static constexpr int kZF = 6;
    static constexpr int kSF = 7;
    static constexpr int kOF = 11;

    void set_flag(int bit, bool value);
    bool get_flag(int bit) const;
    void update_flags_logic(uint64_t result, uint32_t size);
    void update_flags_arith(uint64_t result, uint64_t lhs, uint64_t rhs, uint32_t size,
                            bool subtract);
    void update_flags_incdec(uint64_t result, uint64_t operand, uint32_t size, bool increment);
    void update_flags_shift(uint64_t result, uint32_t size, uint32_t count);

    // Returns true when the result should be written back (false for CMP).
    bool apply_alu(uint8_t aluop, uint64_t& dst, uint64_t src, uint32_t size);

    uint64_t read_reg_sized(uint32_t index, uint32_t size) const;
    void write_reg_sized(uint32_t index, uint64_t value, uint32_t size);
    uint8_t reg8_index(uint32_t rm, bool rex_active) const;

    uint64_t read_mem_val(uint64_t addr, uint32_t size);
    void write_mem_val(uint64_t addr, uint64_t value, uint32_t size);

    bool decode_modrm(uint8_t modrm, uint32_t& reg, uint32_t& rm, bool& is_mem, uint64_t& addr,
                      uint32_t imm_size = 0);

    uint8_t fetch8();
    uint16_t fetch16v();
    uint32_t fetch32v();
    uint64_t fetch_imm(uint32_t size);
    uint64_t fetch_sext_imm8(uint32_t size);
    uint64_t fetch_imm_for_size(uint32_t size);
    uint32_t effective_operand_size() const;
    bool eval_condition(uint8_t cond) const;

    // Opcode-family handlers (each returns false to halt the vCPU).
    bool exec_alu_rm_reg(uint8_t opcode);
    bool exec_mov_rm_reg(uint8_t opcode);
    bool exec_alu_immediate(uint8_t opcode);
    bool exec_group2(uint8_t opcode);
    bool exec_group3(uint8_t opcode);
    bool exec_incdec_group(uint8_t opcode);
    bool exec_push_pop(uint8_t opcode);
    bool exec_two_byte(uint8_t opcode);

    std::shared_ptr<MemoryManager> mem_;
    std::shared_ptr<DeviceBus> bus_;

    std::array<uint64_t, 16> gprs_{};
    uint64_t rip_{0};
    uint64_t rflags_{0x02};
    uint64_t cr0_{0x60000010};
    uint64_t cr3_{0};
    uint64_t cr4_{0};
    CPUMode mode_{CPUMode::REAL_16BIT};

    // Per-instruction prefix state.
    uint8_t rex_{0};
    bool rex_present_{false};
    bool prefix_66_{false};
};

}  // namespace lime

#endif  // LIME_X86_CPU_HPP
