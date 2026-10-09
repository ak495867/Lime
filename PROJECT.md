# Project: LIME Production-Grade Hypervisor Enhancements

## Architecture
LIME is an educational and production-grade hypervisor written in C++20 supporting multi-architecture emulation and virtualization (RISC-V and x86_64 guests).

### Major Subsystems
1. **JIT & Execution Engine (`src/jit_x86.cpp`, `src/vcpu.cpp`)**:
   - Native x86_64 machine code generator using vendored `asmjit` (`third_party/asmjit`).
   - Translates RISC-V basic blocks into host machine code functions (`NativeBlock::EntryFn`).
   - Replaces/accelerates threaded computed-goto interpreter.
2. **x86_64 Target CPU & Decoder (`src/x86_cpu.cpp`, `include/lime/x86_cpu.hpp`)**:
   - Comprehensive software instruction decoder and execution engine.
   - Decodes REX prefixes, ModR/M, SIB, displacements (including RIP-relative), immediates.
   - Executes ALU, bitwise, shifts, control flow (JMP, Jcc, CALL, RET), stack (PUSH, POP), memory access, and flags (CF, PF, AF, ZF, SF, OF, DF).
3. **Cross-Platform Asynchronous I/O Subsystem (`src/async_io.cpp`, `include/lime/async_io.hpp`)**:
   - High-performance non-blocking I/O abstraction layer.
   - Windows: I/O Completion Ports (`IOCPAsyncIOEngine`) with `FILE_FLAG_OVERLAPPED` and `GetQueuedCompletionStatus`.
   - Linux: `URingAsyncIOEngine` using `io_uring`.
   - Fallback: `ThreadPoolAsyncIOEngine`.
   - Integrates with `NVMeController` and `VirtIOBlockDevice` to prevent VCPU starvation during heavy I/O.
4. **Test Suite & Verification (`tests/test_lime.cpp`)**:
   - Self-contained verification suite exercising Memory, ACPI, ECAM, NVMe, x86_64 decoder suite (20 cases), Native JIT suite, and Async I/O non-blocking performance.

## Feature Inventory
| # | Feature | Description | Milestone | Source |
|---|---------|-------------|-----------|--------|
| 1 | R2-Decoder-ModRMSIB | ModR/M and SIB decoder with RIP-relative displacement support | M1 | Survey / R2 |
| 2 | R2-Decoder-ALU-Flags | 64-bit and 32-bit ALU and flag updates (CF, ZF, SF, OF, PF) | M1 | Survey / R2 |
| 3 | R2-Decoder-ControlFlow | JMP, Jcc, CALL, RET instruction decoding and execution | M1 | Survey / R2 |
| 4 | R2-Decoder-BootBase | MOVSXD (0x63), LEA, stack operations for OS boot baseline | M1 | Survey / R2 |
| 5 | R2-Decoder-TestFix | Fix displacement bug in `case_jmp_call_ret` in `tests/test_lime.cpp` | M1 | Survey / R2 |
| 6 | R1-Asmjit-Integration | Verify and hook `asmjit` machine-code emitter into `VCPU` execution loop | M2 | Survey / R1 |
| 7 | R1-JIT-BlockTranslation | BasicBlock translation with register allocation and host ABI conventions | M2 | Survey / R1 |
| 8 | R1-JIT-UnitTest | Verification test in `test_lime.cpp` compiling and executing native code | M2 | Survey / R1 |
| 9 | R3-Async-NVMeDoorbell | Fix NVMe BAR0 offset handling in `NVMeController::write` | M3 | Survey / R3 |
| 10 | R3-Async-QueueAlign | Align SQ/CQ addresses and implement non-blocking `process_io_sq` | M3 | Survey / R3 |
| 11 | R3-Async-VirtIO | Hook `VirtIOBlockDevice` asynchronous requests and polling | M3 | Survey / R3 |
| 12 | R3-Async-TestVerify | Multi-metric async verification test measuring immediate (<500µs) submission | M3 | Survey / R3 |
| 13 | E2E-FullSuite-Pass | End-to-end full build and test execution passing 100% of test cases | M4 | Survey / E2E |

## Milestones
| # | Name | Scope | Dependencies | Status |
|---|------|-------|-------------|--------|
| 1 | M1: x86_64 Target Decoder | `src/x86_cpu.cpp`, `include/lime/x86_cpu.hpp`, `tests/test_lime.cpp` (decoder test suite) | none | IN_PROGRESS |
| 2 | M2: True x86_64 JIT Emitter | `src/jit_x86.cpp`, `include/lime/jit_x86.hpp`, `src/vcpu.cpp`, `tests/test_lime.cpp` (native JIT suite) | none | PLANNED |
| 3 | M3: Async I/O (NVMe & VirtIO) | `src/nvme.cpp`, `include/lime/nvme.hpp`, `src/devices.cpp`, `src/async_io.cpp`, `tests/test_lime.cpp` (async suite) | none | PLANNED |
| 4 | M4: E2E Integration & Verification | Full project build and end-to-end regression validation | M1, M2, M3 | PLANNED |

## Interface Contracts
### `x86_cpu.cpp` ↔ `test_lime.cpp`
- Instruction execution: `Machine::run(size_t max_steps)`
- GPR inspection: `cpu->get_gpr(uint32_t reg_index)`
- Flags inspection: `cpu->get_rflags()`

### `jit_x86.cpp` ↔ `vcpu.cpp`
- `NativeBlock::EntryFn`: `void (*)(VCPU* cpu, uint64_t* regs, uint64_t* pc)`
- Host register mapping: `rbx` = `cpu`, `rsi` = `regs`, `rdi` = `pc`
- `X86JIT::compile(const BasicBlock& bb) -> NativeBlock`

### `async_io.cpp` ↔ `nvme.cpp` / `devices.cpp`
- `AsyncIOEngine::submit(std::shared_ptr<AsyncRequest> req) -> bool`
- `AsyncIOEngine::poll(size_t max_completions) -> size_t`
- `submit_sparse_range(std::shared_ptr<AsyncIOEngine> engine, std::shared_ptr<SparseDisk> disk, ...)`

## Code Layout
- `include/lime/`: Public headers (`x86_cpu.hpp`, `jit_x86.hpp`, `async_io.hpp`, `nvme.hpp`, `devices.hpp`)
- `src/`: Implementation files (`x86_cpu.cpp`, `jit_x86.cpp`, `async_io.cpp`, `nvme.cpp`, `devices.cpp`, `vcpu.cpp`)
- `tests/`: Test harness (`test_lime.cpp`)
- `third_party/asmjit/`: Vendored AsmJit library
- `build.sh`: Primary compilation script
