#ifndef LIME_JIT_X86_HPP
#define LIME_JIT_X86_HPP

#include <cstdint>
#include <cstddef>
#include <memory>

namespace lime {

class VCPU;
struct BasicBlock;

// ---------------------------------------------------------------------------
// NativeBlock — a RISC-V basic block translated to executable x86_64 machine
// code by the asmjit backend.
//
// Emitted entry signature: void(VCPU* cpu, uint64_t* regs, uint64_t* pc)
//   * `regs` points at the vCPU register file (x0..x31).
//   * `pc`   points at the vCPU program counter; on entry it holds the block
//     start address and on exit the address of the next instruction to fetch.
//   * `cpu`  is used for "fallback" instructions (memory, CSR, FP, atomics)
//     that are not inlined; the helper updates the same pc/regs storage.
// ---------------------------------------------------------------------------
struct NativeBlock {
    using EntryFn = void (*)(VCPU* cpu, uint64_t* regs, uint64_t* pc);

    EntryFn entry{nullptr};
    uint64_t start_pc{0};
    size_t op_count{0};
    uint64_t exec_count{0};

    bool valid() const { return entry != nullptr; }
};

// ---------------------------------------------------------------------------
// X86JIT — asmjit-based machine-code emitter for RISC-V basic blocks.
//
// Inlined natively : LUI, AUIPC, OP-IMM, OP, OP-IMM-32, OP-32, MUL/MULW,
//                    JAL, JALR and all conditional branches.
// Helper-called    : loads/stores, CSR/system, FP, atomics, compressed,
//                    M-extension division — one call per instruction.
// ---------------------------------------------------------------------------
class X86JIT {
public:
    X86JIT();
    ~X86JIT();

    // True when the asmjit backend is compiled into this build.
    static bool available();

    // Compiles (or reuses) a native block for `bb`; nullptr if unavailable
    // or the backend failed to assemble the block.
    NativeBlock* lookup_or_compile(const BasicBlock& bb);

    void invalidate_page(uint64_t gpa);
    void clear_cache();
    size_t block_count() const;

    // Compile-time counters: instructions inlined as native machine code vs
    // instructions delegated to the interpreter helper.
    uint64_t native_ops() const { return native_ops_; }
    uint64_t fallback_ops() const { return fallback_ops_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    uint64_t native_ops_{0};
    uint64_t fallback_ops_{0};

    NativeBlock* compile_new(const BasicBlock& bb);
};

// Emitted-code helper: executes one instruction on the interpreter (pc is set
// to inst_pc + 4 first). Returns false when the vCPU stopped or transferred
// control elsewhere, in which case the native block must abort.
bool lime_jit_execute_fallback(VCPU* cpu, uint64_t inst_pc, uint32_t inst);

}  // namespace lime

#endif  // LIME_JIT_X86_HPP
