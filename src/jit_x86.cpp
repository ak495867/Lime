#include "lime/jit_x86.hpp"
#include "lime/jit.hpp"
#include "lime/vcpu.hpp"

#include <mutex>
#include <unordered_map>
#include <vector>

#if defined(__has_include)
#if __has_include(<asmjit/core.h>)
#if __has_include(<asmjit/x86.h>) && (defined(__x86_64__) || defined(_M_X64))
#define LIME_HAVE_ASMJIT_X86 1
#define LIME_HAVE_ASMJIT 1
#include <asmjit/core.h>
#include <asmjit/x86.h>
#elif __has_include(<asmjit/a64.h>) && (defined(__aarch64__) || defined(_M_ARM64))
#define LIME_HAVE_ASMJIT_A64 1
#define LIME_HAVE_ASMJIT 1
#include <asmjit/core.h>
#include <asmjit/a64.h>
#endif
#endif
#endif

namespace lime {

#ifdef LIME_HAVE_ASMJIT

using namespace asmjit;

#if defined(LIME_HAVE_ASMJIT_X86)

namespace {

// Host register plan inside emitted blocks:
//   rbx = VCPU*,  r12 = regs base,  r13 = pc pointer  (callee-saved on both Win64 and SysV)
//   rax/rcx/rdx/r8..r11 = scratch (caller-saved, safe across helper calls)
constexpr x86::Gp kHostCpu = x86::rbx;
constexpr x86::Gp kHostRegs = x86::r12;
constexpr x86::Gp kHostPc = x86::r13;
constexpr x86::Gp kScratch = x86::rax;
constexpr x86::Gp kScratch2 = x86::r9;

inline void load_reg(x86::Assembler& a, const x86::Gp& dst, uint32_t riscv_reg) {
    if (riscv_reg == 0) {
        a.xor_(dst.r32(), dst.r32());
    } else {
        a.mov(dst, x86::qword_ptr(kHostRegs, static_cast<int32_t>(riscv_reg * 8)));
    }
}

inline void store_reg(x86::Assembler& a, uint32_t riscv_reg, const x86::Gp& src) {
    if (riscv_reg != 0) {
        a.mov(x86::qword_ptr(kHostRegs, static_cast<int32_t>(riscv_reg * 8)), src);
    }
}

inline void emit_fallback(x86::Assembler& a, uint64_t inst_pc, uint32_t inst, const Label& abort_label) {
#if defined(_WIN32)
    a.mov(x86::rcx, kHostCpu);  // helper arg 1: cpu
    a.mov(x86::rdx, Imm(static_cast<int64_t>(inst_pc)));
    a.mov(x86::r8d, Imm(static_cast<int32_t>(inst)));
#else
    a.mov(x86::rdi, kHostCpu);  // helper arg 1: cpu
    a.mov(x86::rsi, Imm(static_cast<int64_t>(inst_pc)));
    a.mov(x86::edx, Imm(static_cast<int32_t>(inst)));
#endif
    a.mov(kScratch, Imm(reinterpret_cast<int64_t>(&lime_jit_execute_fallback)));
    a.call(kScratch);
    a.test(x86::al, x86::al);
    a.j(x86::CondCode::kZero, abort_label);  // halted / control transferred -> leave the block
}

// Inlines an OP-IMM (0x13) / OP (0x33) ALU instruction; returns false if the
// instruction must go through the interpreter helper instead.
inline bool emit_alu(x86::Assembler& a, const MicroOp& op, bool imm_form) {
    const uint32_t funct3 = (op.raw_inst >> 12) & 0x7;
    const uint32_t funct7 = (op.raw_inst >> 25) & 0x7F;
    const bool is_32bit = (op.opcode == 0x1B || op.opcode == 0x3B);

    if (!imm_form && funct7 == 0x01) {
        // M extension: only multiply is safe to inline (no traps).
        if (funct3 != 0) return false;
        load_reg(a, kScratch, op.rs1);
        load_reg(a, kScratch2, op.rs2);
        if (is_32bit) {
            a.imul(kScratch.r32(), kScratch2.r32());
            a.movsxd(kScratch, kScratch.r32());
        } else {
            a.imul(kScratch, kScratch2);
        }
        store_reg(a, op.rd, kScratch);
        return true;
    }

    load_reg(a, kScratch, op.rs1);
    if (imm_form) {
        a.mov(kScratch2, Imm(static_cast<int64_t>(op.imm)));
    } else {
        load_reg(a, kScratch2, op.rs2);
    }

    switch (funct3) {
    case 0:  // ADD / ADDI (SUB when funct7 bit 0x20 on register form)
        if (!imm_form && (funct7 & 0x20)) {
            if (is_32bit) {
                a.sub(kScratch.r32(), kScratch2.r32());
                a.movsxd(kScratch, kScratch.r32());
            } else {
                a.sub(kScratch, kScratch2);
            }
        } else {
            if (is_32bit) {
                a.add(kScratch.r32(), kScratch2.r32());
                a.movsxd(kScratch, kScratch.r32());
            } else {
                a.add(kScratch, kScratch2);
            }
        }
        break;
    case 1:  // SLL / SLLI
        a.and_(kScratch2.r32(), is_32bit ? Imm(0x1F) : Imm(0x3F));
        a.mov(x86::rcx, kScratch2);
        if (is_32bit) {
            a.shl(kScratch.r32(), x86::cl);
            a.movsxd(kScratch, kScratch.r32());
        } else {
            a.shl(kScratch, x86::cl);
        }
        break;
    case 2:  // SLT / SLTI
        a.cmp(kScratch, kScratch2);
        a.set(x86::CondCode::kSignedLT, x86::al);
        a.movzx(kScratch, x86::al);
        break;
    case 3:  // SLTU / SLTIU
        a.cmp(kScratch, kScratch2);
        a.set(x86::CondCode::kUnsignedLT, x86::al);
        a.movzx(kScratch, x86::al);
        break;
    case 4:  // XOR / XORI
        a.xor_(kScratch, kScratch2);
        break;
    case 5:  // SRL / SRA / SRLI / SRAI
        if (!imm_form && (funct7 & 0x20)) {
            a.and_(kScratch2.r32(), is_32bit ? Imm(0x1F) : Imm(0x3F));
            a.mov(x86::rcx, kScratch2);
            if (is_32bit) {
                a.sar(kScratch.r32(), x86::cl);
                a.movsxd(kScratch, kScratch.r32());
            } else {
                a.sar(kScratch, x86::cl);
            }
        } else {
            a.and_(kScratch2.r32(), is_32bit ? Imm(0x1F) : Imm(0x3F));
            a.mov(x86::rcx, kScratch2);
            if (is_32bit) {
                a.shr(kScratch.r32(), x86::cl);
                a.movsxd(kScratch, kScratch.r32());
            } else {
                a.shr(kScratch, x86::cl);
            }
        }
        break;
    case 6:  // OR / ORI
        a.or_(kScratch, kScratch2);
        break;
    case 7:  // AND / ANDI
        a.and_(kScratch, kScratch2);
        break;
    default:
        return false;
    }

    store_reg(a, op.rd, kScratch);
    return true;
}

inline x86::CondCode branch_cond(uint32_t funct3) {
    switch (funct3) {
    case 0: return x86::CondCode::kEqual;
    case 1: return x86::CondCode::kNotEqual;
    case 4: return x86::CondCode::kSignedLT;
    case 5: return x86::CondCode::kSignedGE;
    case 6: return x86::CondCode::kUnsignedLT;
    case 7: return x86::CondCode::kUnsignedGE;
    default: return x86::CondCode::kEqual;
    }
}

}  // namespace

#elif defined(LIME_HAVE_ASMJIT_A64)

namespace {

constexpr a64::Gp kHostCpu = a64::x19;
constexpr a64::Gp kHostRegs = a64::x20;
constexpr a64::Gp kHostPc = a64::x21;
constexpr a64::Gp kScratch = a64::x9;
constexpr a64::Gp kScratch2 = a64::x10;

inline void load_reg(a64::Assembler& a, const a64::Gp& dst, uint32_t riscv_reg) {
    if (riscv_reg == 0) {
        a.mov(dst, 0);
    } else {
        a.ldr(dst, a64::ptr(kHostRegs, static_cast<int32_t>(riscv_reg * 8)));
    }
}

inline void store_reg(a64::Assembler& a, uint32_t riscv_reg, const a64::Gp& src) {
    if (riscv_reg != 0) {
        a.str(src, a64::ptr(kHostRegs, static_cast<int32_t>(riscv_reg * 8)));
    }
}

inline void emit_fallback(a64::Assembler& a, uint64_t inst_pc, uint32_t inst, const Label& abort_label) {
    a.mov(a64::x0, kHostCpu);
    a.mov(a64::x1, inst_pc);
    a.mov(a64::w2, inst);
    a.mov(kScratch, reinterpret_cast<uintptr_t>(&lime_jit_execute_fallback));
    a.blr(kScratch);
    a.cbz(a64::x0, abort_label);
}

}  // namespace

#endif

struct X86JIT::Impl {
    JitRuntime rt;
    std::unordered_map<uint64_t, NativeBlock> cache;
    std::mutex mutex;
};

X86JIT::X86JIT() : impl_(new Impl()) {}
X86JIT::~X86JIT() = default;

bool X86JIT::available() {
#if defined(LIME_HAVE_ASMJIT_X86) || defined(LIME_HAVE_ASMJIT_A64)
    return true;
#else
    return false;
#endif
}

NativeBlock* X86JIT::compile_new(const BasicBlock& bb) {
#if defined(LIME_HAVE_ASMJIT_X86)
    CodeHolder code;
    if (impl_->rt.environment().arch() != Arch::kX64) return nullptr;
    if (code.init(impl_->rt.environment()) != kErrorOk) return nullptr;

    x86::Assembler a(&code);
    const uint64_t block_pc = bb.start_pc;
    const size_t n = bb.ops.size();

    // Prologue: save callee-saved hosts, stash args.
    a.push(x86::rbx);
    a.push(x86::r12);
    a.push(x86::r13);
#if defined(_WIN32)
    // Windows x64: rcx/rdx/r8, 32-byte shadow space.
    a.sub(x86::rsp, 32);
    a.mov(kHostCpu, x86::rcx);
    a.mov(kHostRegs, x86::rdx);
    a.mov(kHostPc, x86::r8);
#else
    // System V AMD64 (Linux / macOS): rdi/rsi/rdx.
    // 3 pushes (24B) + return addr (8B) = 32B stack alignment.
    a.mov(kHostCpu, x86::rdi);
    a.mov(kHostRegs, x86::rsi);
    a.mov(kHostPc, x86::rdx);
#endif

    Label abort_label = a.new_label();
    Label end_label = a.new_label();
    bool terminator_handled = false;
    size_t native_count = 0;
    size_t fallback_count = 0;

    for (size_t i = 0; i < n; ++i) {
        const MicroOp& op = bb.ops[i];
        const uint64_t inst_pc = block_pc + i * 4;
        const uint32_t funct3 = (op.raw_inst >> 12) & 0x7;
        const uint32_t funct7 = (op.raw_inst >> 25) & 0x7F;

        if ((op.raw_inst & 0x3) != 0x3) {
            // Compressed instruction: interpreter (block fetch is 4-byte wide).
            emit_fallback(a, inst_pc, op.raw_inst, abort_label);
            fallback_count++;
            continue;
        }

        switch (op.opcode) {
        case 0x37: {  // LUI
            a.mov(kScratch, Imm(static_cast<int64_t>(static_cast<int32_t>(op.raw_inst & 0xFFFFF000u))));
            store_reg(a, op.rd, kScratch);
            native_count++;
            break;
        }
        case 0x17: {  // AUIPC
            uint64_t value = inst_pc + static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(op.raw_inst & 0xFFFFF000u)));
            a.mov(kScratch, Imm(static_cast<int64_t>(value)));
            store_reg(a, op.rd, kScratch);
            native_count++;
            break;
        }
        case 0x13:  // OP-IMM
        case 0x1B:  // OP-IMM-32
            if (emit_alu(a, op, /*imm_form=*/true)) {
                native_count++;
            } else {
                emit_fallback(a, inst_pc, op.raw_inst, abort_label);
                fallback_count++;
            }
            break;
        case 0x33:  // OP
        case 0x3B:  // OP-32
            if (emit_alu(a, op, /*imm_form=*/false)) {
                native_count++;
            } else {
                emit_fallback(a, inst_pc, op.raw_inst, abort_label);
                fallback_count++;
            }
            break;
        case 0x0F:  // FENCE: no-op on this single-hart model
            native_count++;
            break;
        case 0x6F: {  // JAL
            int32_t imm_j = ((static_cast<int32_t>(op.raw_inst) >> 31) << 20) |
                            (((op.raw_inst >> 12) & 0xFF) << 12) |
                            (((op.raw_inst >> 20) & 0x01) << 11) |
                            (((op.raw_inst >> 21) & 0x3FF) << 1);
            if (op.rd != 0) {
                a.mov(kScratch, Imm(static_cast<int64_t>(inst_pc + 4)));
                store_reg(a, op.rd, kScratch);
            }
            a.mov(x86::qword_ptr(kHostPc), Imm(static_cast<int64_t>(inst_pc + static_cast<uint64_t>(imm_j))));
            a.jmp(end_label);
            terminator_handled = true;
            native_count++;
            break;
        }
        case 0x67: {  // JALR
            load_reg(a, kScratch, op.rs1);
            a.add(kScratch, Imm(static_cast<int64_t>(op.imm)));
            a.and_(kScratch, Imm(-2));
            a.mov(kScratch2, kScratch);
            if (op.rd != 0) {
                a.mov(kScratch, Imm(static_cast<int64_t>(inst_pc + 4)));
                store_reg(a, op.rd, kScratch);
            }
            a.mov(x86::qword_ptr(kHostPc), kScratch2);
            a.jmp(end_label);
            terminator_handled = true;
            native_count++;
            break;
        }
        case 0x63: {  // conditional branch (always terminates the block)
            load_reg(a, kScratch, op.rs1);
            load_reg(a, kScratch2, op.rs2);
            a.cmp(kScratch, kScratch2);

            int32_t imm_b = ((static_cast<int32_t>(op.raw_inst) >> 31) << 12) |
                            (((op.raw_inst >> 25) & 0x3F) << 5) |
                            (((op.raw_inst >> 8) & 0x0F) << 1) |
                            (((op.raw_inst >> 7) & 0x01) << 11);

            Label taken = a.new_label();
            a.j(branch_cond(funct3), taken);
            a.mov(x86::qword_ptr(kHostPc), Imm(static_cast<int64_t>(inst_pc + 4)));
            a.jmp(end_label);
            a.bind(taken);
            a.mov(x86::qword_ptr(kHostPc), Imm(static_cast<int64_t>(inst_pc + static_cast<uint64_t>(imm_b))));
            terminator_handled = true;
            native_count++;
            break;
        }
        default:
            // Loads/stores, CSR/system, FP, atomics -> interpreter helper.
            emit_fallback(a, inst_pc, op.raw_inst, abort_label);
            fallback_count++;
            if (op.opcode == 0x73) {
                terminator_handled = true;  // helper owns pc for SYSTEM ops
            }
            break;
        }
    }

    // Sequential exit: advance pc past the block when no terminator owns it.
    if (!terminator_handled) {
        a.mov(x86::qword_ptr(kHostPc), Imm(static_cast<int64_t>(block_pc + 4 * n)));
    }

    a.bind(end_label);
#if defined(_WIN32)
    a.add(x86::rsp, 32);
#endif
    a.pop(x86::r13);
    a.pop(x86::r12);
    a.pop(x86::rbx);
    a.ret();

    // Abort path: leave immediately; pc was updated by the helper.
    a.bind(abort_label);
#if defined(_WIN32)
    a.add(x86::rsp, 32);
#endif
    a.pop(x86::r13);
    a.pop(x86::r12);
    a.pop(x86::rbx);
    a.ret();

    void* fn = nullptr;
    if (impl_->rt.add(&fn, &code) != kErrorOk) return nullptr;

    NativeBlock nb;
    nb.entry = reinterpret_cast<NativeBlock::EntryFn>(fn);
    nb.start_pc = block_pc;
    nb.op_count = n;

    native_ops_ += native_count;
    fallback_ops_ += fallback_count;

    auto res = impl_->cache.emplace(block_pc, nb);
    return &res.first->second;
#elif defined(LIME_HAVE_ASMJIT_A64)
    CodeHolder code;
    if (impl_->rt.environment().arch() != Arch::kAArch64) return nullptr;
    if (code.init(impl_->rt.environment()) != kErrorOk) return nullptr;

    a64::Assembler a(&code);
    const uint64_t block_pc = bb.start_pc;
    const size_t n = bb.ops.size();

    // Prologue: save x29/x30, x19/x20, x21
    a.stp(a64::x29, a64::x30, a64::ptr_pre(a64::sp, -32));
    a.mov(a64::x29, a64::sp);
    a.stp(kHostCpu, kHostRegs, a64::ptr_pre(a64::sp, -32));
    a.str(kHostPc, a64::ptr_pre(a64::sp, -16));

    // Stash arguments: x0=cpu, x1=regs, x2=pc
    a.mov(kHostCpu, a64::x0);
    a.mov(kHostRegs, a64::x1);
    a.mov(kHostPc, a64::x2);

    Label abort_label = a.new_label();
    Label end_label = a.new_label();
    bool terminator_handled = false;
    size_t native_count = 0;
    size_t fallback_count = 0;

    for (size_t i = 0; i < n; ++i) {
        const MicroOp& op = bb.ops[i];
        const uint64_t inst_pc = block_pc + i * 4;
        const uint32_t funct3 = (op.raw_inst >> 12) & 0x7;
        const uint32_t funct7 = (op.raw_inst >> 25) & 0x7F;

        if ((op.raw_inst & 0x3) != 0x3) {
            emit_fallback(a, inst_pc, op.raw_inst, abort_label);
            fallback_count++;
            continue;
        }

        switch (op.opcode) {
        case 0x37: { // LUI
            a.mov(kScratch, static_cast<int64_t>(static_cast<int32_t>(op.imm)));
            store_reg(a, op.rd, kScratch);
            native_count++;
            break;
        }
        case 0x17: { // AUIPC
            a.mov(kScratch, static_cast<int64_t>(static_cast<int32_t>(inst_pc + op.imm)));
            store_reg(a, op.rd, kScratch);
            native_count++;
            break;
        }
        case 0x6F: { // JAL
            if (op.rd != 0) {
                a.mov(kScratch, static_cast<int64_t>(inst_pc + 4));
                store_reg(a, op.rd, kScratch);
            }
            a.mov(kScratch, static_cast<int64_t>(inst_pc + op.imm));
            a.str(kScratch, a64::ptr(kHostPc));
            a.b(end_label);
            terminator_handled = true;
            native_count++;
            break;
        }
        case 0x67: { // JALR
            load_reg(a, kScratch, op.rs1);
            if (op.imm != 0) a.add(kScratch, kScratch, op.imm);
            a.and_(kScratch, kScratch, ~1ULL);
            if (op.rd != 0) {
                a.mov(kScratch2, static_cast<int64_t>(inst_pc + 4));
                store_reg(a, op.rd, kScratch2);
            }
            a.str(kScratch, a64::ptr(kHostPc));
            a.b(end_label);
            terminator_handled = true;
            native_count++;
            break;
        }
        case 0x63: { // BRANCH
            load_reg(a, kScratch, op.rs1);
            load_reg(a, kScratch2, op.rs2);
            a.cmp(kScratch, kScratch2);
            Label not_taken = a.new_label();
            switch (funct3) {
            case 0: a.b_ne(not_taken); break;
            case 1: a.b_eq(not_taken); break;
            case 4: a.b_ge(not_taken); break;
            case 5: a.b_lt(not_taken); break;
            case 6: a.b_hs(not_taken); break;
            case 7: a.b_lo(not_taken); break;
            default: a.b_ne(not_taken); break;
            }
            a.mov(kScratch, static_cast<int64_t>(inst_pc + op.imm));
            a.str(kScratch, a64::ptr(kHostPc));
            a.b(end_label);
            a.bind(not_taken);
            native_count++;
            break;
        }
        case 0x13: { // OP-IMM
            load_reg(a, kScratch, op.rs1);
            switch (funct3) {
            case 0: a.add(kScratch, kScratch, op.imm); break;
            case 2:
                a.cmp(kScratch, op.imm);
                a.cset(kScratch, a64::CondCode::kLT);
                break;
            case 3:
                a.cmp(kScratch, static_cast<uint64_t>(static_cast<int64_t>(op.imm)));
                a.cset(kScratch, a64::CondCode::kLO);
                break;
            case 4: a.eor(kScratch, kScratch, op.imm); break;
            case 6: a.orr(kScratch, kScratch, op.imm); break;
            case 7: a.and_(kScratch, kScratch, op.imm); break;
            case 1: a.lsl(kScratch, kScratch, op.imm & 0x3F); break;
            case 5:
                if (funct7 & 0x20) a.asr(kScratch, kScratch, op.imm & 0x3F);
                else a.lsr(kScratch, kScratch, op.imm & 0x3F);
                break;
            }
            store_reg(a, op.rd, kScratch);
            native_count++;
            break;
        }
        case 0x33: { // OP
            load_reg(a, kScratch, op.rs1);
            load_reg(a, kScratch2, op.rs2);
            if (funct7 == 1) {
                if (funct3 == 0) a.mul(kScratch, kScratch, kScratch2);
                else {
                    emit_fallback(a, inst_pc, op.raw_inst, abort_label);
                    fallback_count++;
                    break;
                }
            } else {
                switch (funct3) {
                case 0:
                    if (funct7 & 0x20) a.sub(kScratch, kScratch, kScratch2);
                    else a.add(kScratch, kScratch, kScratch2);
                    break;
                case 1: a.lsl(kScratch, kScratch, kScratch2); break;
                case 2:
                    a.cmp(kScratch, kScratch2);
                    a.cset(kScratch, a64::CondCode::kLT);
                    break;
                case 3:
                    a.cmp(kScratch, kScratch2);
                    a.cset(kScratch, a64::CondCode::kLO);
                    break;
                case 4: a.eor(kScratch, kScratch, kScratch2); break;
                case 5:
                    if (funct7 & 0x20) a.asr(kScratch, kScratch, kScratch2);
                    else a.lsr(kScratch, kScratch, kScratch2);
                    break;
                case 6: a.orr(kScratch, kScratch, kScratch2); break;
                case 7: a.and_(kScratch, kScratch, kScratch2); break;
                }
            }
            store_reg(a, op.rd, kScratch);
            native_count++;
            break;
        }
        case 0x1B: { // OP-IMM-32
            load_reg(a, kScratch, op.rs1);
            switch (funct3) {
            case 0: a.add(kScratch.w(), kScratch.w(), op.imm); break;
            case 1: a.lsl(kScratch.w(), kScratch.w(), op.imm & 0x1F); break;
            case 5:
                if (funct7 & 0x20) a.asr(kScratch.w(), kScratch.w(), op.imm & 0x1F);
                else a.lsr(kScratch.w(), kScratch.w(), op.imm & 0x1F);
                break;
            }
            a.sxtw(kScratch, kScratch.w());
            store_reg(a, op.rd, kScratch);
            native_count++;
            break;
        }
        case 0x3B: { // OP-32
            load_reg(a, kScratch, op.rs1);
            load_reg(a, kScratch2, op.rs2);
            if (funct7 == 1 && funct3 == 0) {
                a.mul(kScratch.w(), kScratch.w(), kScratch2.w());
            } else if (funct3 == 0) {
                if (funct7 & 0x20) a.sub(kScratch.w(), kScratch.w(), kScratch2.w());
                else a.add(kScratch.w(), kScratch.w(), kScratch2.w());
            } else if (funct3 == 1) {
                a.lsl(kScratch.w(), kScratch.w(), kScratch2.w());
            } else if (funct3 == 5) {
                if (funct7 & 0x20) a.asr(kScratch.w(), kScratch.w(), kScratch2.w());
                else a.lsr(kScratch.w(), kScratch.w(), kScratch2.w());
            }
            a.sxtw(kScratch, kScratch.w());
            store_reg(a, op.rd, kScratch);
            native_count++;
            break;
        }
        default:
            emit_fallback(a, inst_pc, op.raw_inst, abort_label);
            fallback_count++;
            break;
        }

        if (terminator_handled) break;
    }

    if (!terminator_handled) {
        a.mov(kScratch, static_cast<int64_t>(block_pc + n * 4));
        a.str(kScratch, a64::ptr(kHostPc));
    }

    a.bind(abort_label);
    a.bind(end_label);

    a.ldr(kHostPc, a64::ptr_post(a64::sp, 16));
    a.ldp(kHostCpu, kHostRegs, a64::ptr_post(a64::sp, 32));
    a.ldp(a64::x29, a64::x30, a64::ptr_post(a64::sp, 32));
    a.ret();

    void* fn = nullptr;
    if (impl_->rt.add(&fn, &code) != kErrorOk) return nullptr;

    NativeBlock nb;
    nb.entry = reinterpret_cast<NativeBlock::EntryFn>(fn);
    nb.start_pc = block_pc;
    nb.op_count = n;

    native_ops_ += native_count;
    fallback_ops_ += fallback_count;

    auto res = impl_->cache.emplace(block_pc, nb);
    return &res.first->second;
#else
    (void)bb;
    return nullptr;
#endif
}

NativeBlock* X86JIT::lookup_or_compile(const BasicBlock& bb) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto it = impl_->cache.find(bb.start_pc);
    if (it != impl_->cache.end()) {
        it->second.exec_count++;
        return &it->second;
    }
    return compile_new(bb);
}

void X86JIT::invalidate_page(uint64_t gpa) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    uint64_t page_base = gpa & ~0xFFFULL;
    for (auto it = impl_->cache.begin(); it != impl_->cache.end();) {
        if ((it->first & ~0xFFFULL) == page_base) {
            impl_->rt.release(reinterpret_cast<void*>(it->second.entry));
            it = impl_->cache.erase(it);
        } else {
            ++it;
        }
    }
}

void X86JIT::clear_cache() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    for (auto& kv : impl_->cache) {
        impl_->rt.release(reinterpret_cast<void*>(kv.second.entry));
    }
    impl_->cache.clear();
}

size_t X86JIT::block_count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->cache.size();
}

bool lime_jit_execute_fallback(VCPU* cpu, uint64_t inst_pc, uint32_t inst) {
    if (!cpu) return false;
    cpu->jit_execute_instruction(inst_pc, inst);
    // Continue only while the vCPU is running and the instruction did not
    // redirect control (trap vector, branch target, ...).
    return cpu->state() == VCPUState::RUNNING && cpu->get_pc() == inst_pc + 4;
}

#else  // !LIME_HAVE_ASMJIT

struct X86JIT::Impl {
    // No backend compiled in: every lookup reports unavailable.
};

X86JIT::X86JIT() : impl_(new Impl()) {}
X86JIT::~X86JIT() = default;

bool X86JIT::available() { return false; }
NativeBlock* X86JIT::compile_new(const BasicBlock&) { return nullptr; }
NativeBlock* X86JIT::lookup_or_compile(const BasicBlock&) { return nullptr; }
void X86JIT::invalidate_page(uint64_t) {}
void X86JIT::clear_cache() {}
size_t X86JIT::block_count() const { return 0; }

bool lime_jit_execute_fallback(VCPU*, uint64_t, uint32_t) { return false; }

#endif  // LIME_HAVE_ASMJIT

}  // namespace lime
