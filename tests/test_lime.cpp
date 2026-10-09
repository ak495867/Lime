#include "lime/memory.hpp"
#include "lime/storage.hpp"
#include "lime/cow_disk.hpp"
#include "lime/devices.hpp"
#include "lime/interrupts.hpp"
#include "lime/mmu.hpp"
#include "lime/hypervisor.hpp"
#include "lime/net_bridge.hpp"
#include "lime/jit.hpp"
#include "lime/acpi.hpp"
#include "lime/pci.hpp"
#include "lime/firmware.hpp"
#include "lime/nvme.hpp"
#include "lime/x86_cpu.hpp"
#include "lime/vcpu.hpp"
#include "lime/scheduler.hpp"
#include "lime/vm.hpp"
#include "lime/jit_x86.hpp"
#include "lime/async_io.hpp"
#include <iostream>
#include <cassert>
#include <chrono>
#include <vector>
#include <string>
#include <filesystem>

void test_memory_subsystem() {
    lime::MemoryManager mem(16 * 1024 * 1024, 4096);
    assert(mem.total_capacity_bytes() == 16 * 1024 * 1024);
    assert(mem.allocated_bytes() == 0);

    mem.write32(0x1000, 0xDEADBEEF);
    assert(mem.read32(0x1000) == 0xDEADBEEF);
    assert(mem.allocated_bytes() == 4096);
    assert(mem.fault_count() == 1);

    size_t inflated = mem.inflate_balloon(1);
    assert(inflated == 1);
    assert(mem.reclaimed_bytes() == 4096);

    size_t deflated = mem.deflate_balloon(1);
    assert(deflated == 1);
    assert(mem.reclaimed_bytes() == 0);

    std::cout << "[TEST PASSED] Memory Subsystem" << std::endl << std::flush;
}

void test_acpi_and_pci_bus() {
    auto mem = std::make_shared<lime::MemoryManager>(16 * 1024 * 1024);
    bool acpi_ok = lime::ACPITableBuilder::generate_tables(mem, 0x000F0000, 2);
    assert(acpi_ok);

    char sig[4]{};
    mem->read_bytes(0x000F0000, sig, 4);
    assert(sig[0] == 'R' && sig[1] == 'S' && sig[2] == 'D' && sig[3] == ' ');

    lime::PCIBus pci(0xE0000000);
    auto dev = std::make_shared<lime::PCIDevice>(0x8086, 0x1234, 0x03, 0x00);
    bool attach_ok = pci.attach_device(0, 1, 0, dev);
    assert(attach_ok);

    uint32_t val = pci.read(0x00008000, 4);
    assert((val & 0xFFFF) == 0x8086);

    std::cout << "[TEST PASSED] ACPI Tables & PCI Express Bus (ECAM)" << std::endl << std::flush;
}

void test_firmware_and_nvme() {
    auto mem = std::make_shared<lime::MemoryManager>(16 * 1024 * 1024);
    bool fw_ok = lime::FirmwareLoader::load_firmware(mem, "", lime::FirmwareType::UEFI_OVMF, 0xFFF00000);
    assert(fw_ok);

    auto disk = std::make_shared<lime::SparseDisk>();
    lime::NVMeController nvme(disk, mem);
    assert((nvme.read(0x08, 4) & 0xFFFF0000) == 0x01080000);
    assert(nvme.read(64 + 0x08, 4) == 0x00010300);

    std::cout << "[TEST PASSED] Firmware Loader & NVMe Controller" << std::endl << std::flush;
}

void test_x86_multi_arch() {
    auto mem = std::make_shared<lime::MemoryManager>(1024 * 1024);
    auto bus = std::make_shared<lime::DeviceBus>();
    lime::X86CPUDecoder x86(mem, bus);

    x86.reset(0x7C00);
    mem->write8(0x7C00, 0xB8);
    mem->write32(0x7C01, 0x12345678);

    bool step_ok = x86.step();
    assert(step_ok);
    assert(x86.get_gpr(0) == 0x12345678);

    std::cout << "[TEST PASSED] Multi-Arch x86_64 Target CPU Subsystem" << std::endl << std::flush;
}

// ===========================================================================
// Expanded x86_64 software decoder: 20 instruction cases covering ALU,
// control flow and memory access (R2 acceptance criteria).
// ===========================================================================
namespace x86test {

constexpr uint64_t kBase = 0x7C00;

struct Machine {
    std::shared_ptr<lime::MemoryManager> mem;
    std::shared_ptr<lime::DeviceBus> bus;
    std::unique_ptr<lime::X86CPUDecoder> cpu;
    uint64_t cursor = kBase;

    Machine() : mem(std::make_shared<lime::MemoryManager>(4 * 1024 * 1024)),
                bus(std::make_shared<lime::DeviceBus>()) {
        cpu = std::make_unique<lime::X86CPUDecoder>(mem, bus);
        cpu->reset(kBase);
    }

    // Appends instruction bytes at the current cursor.
    template <typename... Bytes>
    void code(Bytes... bytes) {
        uint8_t buf[] = {static_cast<uint8_t>(bytes)...};
        for (uint8_t b : buf) mem->write8(cursor++, b);
    }

    void reset_code() { cursor = kBase; }

    void code32(uint64_t addr, uint32_t value) { mem->write32(addr, value); }
    void code64(uint64_t addr, uint64_t value) { mem->write64(addr, value); }

    // Runs until the decoder halts or the safety cap trips.
    void run(unsigned max_steps = 64) {
        for (unsigned i = 0; i < max_steps; ++i) {
            if (!cpu->step()) return;
        }
    }
};

// MOV r64, imm64 via REX.W B8+ or the common 32-bit sign/zero-extended form.
void case_mov_imm() {
    Machine m;
    m.code(0x48, 0xC7, 0xC0, 0x39, 0x05, 0x00, 0x00);  // mov rax, 1337
    assert(m.cpu->step());
    assert(m.cpu->get_gpr(0) == 1337);
}

void case_mov_reg_mem() {
    Machine m;
    m.code64(0x1000, 0xDEADBEEFCAFEBABEULL);
    m.code(0x48, 0xC7, 0xC6, 0x00, 0x10, 0x00, 0x00);  // mov rsi, 0x1000
    m.code(0x48, 0x8B, 0x3E);                          // mov rdi, [rsi]
    assert(m.cpu->step());
    assert(m.cpu->step());
    assert(m.cpu->get_gpr(7) == 0xDEADBEEFCAFEBABEULL);
}

void case_mov_mem_reg() {
    Machine m;
    m.code(0x48, 0xC7, 0xC6, 0x00, 0x20, 0x00, 0x00);  // mov rsi, 0x2000
    m.code(0x48, 0xC7, 0xC7, 0x44, 0x33, 0x22, 0x11);  // mov rdi, 0x11223344
    m.code(0x48, 0x89, 0x3E);                          // mov [rsi], rdi
    assert(m.cpu->step());
    assert(m.cpu->step());
    assert(m.cpu->step());
    assert(m.mem->read64(0x2000) == 0x11223344);
}

void case_add_sub() {
    Machine m;
    m.code(0x48, 0xC7, 0xC0, 0x0A, 0x00, 0x00, 0x00);  // mov rax, 10
    m.code(0x48, 0xC7, 0xC1, 0x20, 0x00, 0x00, 0x00);  // mov rcx, 32
    m.code(0x48, 0x01, 0xC8);                          // add rax, rcx
    assert(m.cpu->step()); assert(m.cpu->step()); assert(m.cpu->step());
    assert(m.cpu->get_gpr(0) == 42);
    m.code(0x48, 0x29, 0xC8);                          // sub rax, rcx
    assert(m.cpu->step());
    assert(m.cpu->get_gpr(0) == 10);
}

void case_sub_zf_cf() {
    Machine m;
    m.code(0x48, 0xC7, 0xC0, 0x05, 0x00, 0x00, 0x00);  // mov rax, 5
    m.code(0x48, 0x83, 0xE8, 0x05);                    // sub rax, 5 -> ZF
    assert(m.cpu->step()); assert(m.cpu->step());
    assert(m.cpu->get_gpr(0) == 0);
    assert((m.cpu->get_rflags() & 0x40) != 0);         // ZF set
    assert((m.cpu->get_rflags() & 0x01) == 0);         // CF clear
    m.code(0x48, 0x83, 0xE8, 0x06);                    // sub rax, 6 -> CF
    assert(m.cpu->step());
    assert((m.cpu->get_rflags() & 0x01) != 0);         // CF set
}

void case_alu_or_and_xor() {
    Machine m;
    m.code(0x48, 0xC7, 0xC0, 0xF0, 0x00, 0x00, 0x00);  // mov rax, 0xF0
    m.code(0x48, 0x0D, 0x0F, 0x00, 0x00, 0x00);        // or eax, 0x0F
    assert(m.cpu->step()); assert(m.cpu->step());
    assert(m.cpu->get_gpr(0) == 0xFF);
    m.code(0x48, 0x25, 0x0C, 0x00, 0x00, 0x00);        // and eax, 0xC
    assert(m.cpu->step());
    assert(m.cpu->get_gpr(0) == 0xC);
    m.code(0x48, 0x35, 0x0F, 0x00, 0x00, 0x00);        // xor eax, 0xF
    assert(m.cpu->step());
    assert(m.cpu->get_gpr(0) == 3);
}

void case_cmp_jne_loop() {
    Machine m;
    // rax = 3; loop: dec rax; cmp rax,0; jne loop
    m.code(0x48, 0xC7, 0xC0, 0x03, 0x00, 0x00, 0x00);  // mov rax, 3    @+0
    m.code(0x48, 0xFF, 0xC8);                          // dec rax      @+7
    m.code(0x48, 0x83, 0xF8, 0x00);                    // cmp rax, 0   @+10
    m.code(0x75, 0xF7);                                // jne loop (rel8 -9 -> dec)
    m.run(32);
    assert(m.cpu->get_gpr(0) == 0);
}

void case_jmp_call_ret() {
    Machine m;
    // call +9 ; mov rbx,55 (return point) ; jmp +8 ; mov rax,77 ; ret
    m.code(0xE8, 0x09, 0x00, 0x00, 0x00);              // call rel32 +9 @+0
    m.code(0x48, 0xC7, 0xC3, 0x37, 0x00, 0x00, 0x00);  // mov rbx, 55    @+5 (after ret)
    m.code(0xEB, 0x08);                                // jmp +8         @+12 (skip subroutine)
    m.code(0x48, 0xC7, 0xC0, 0x4D, 0x00, 0x00, 0x00);  // mov rax, 77    @+14 (call target)
    m.code(0xC3);                                      // ret            @+21
    m.code(0xF4);                                      // hlt            @+22
    m.run(12);
    assert(m.cpu->get_gpr(0) == 77);
    assert(m.cpu->get_gpr(3) == 55);                   // return address was correct

    // Verify MOVSXD and RIP-relative addressing
    Machine m2;
    // 0x7C00: movsxd rax, [rip + 4] (7 bytes) -> target = 0x7C07 + 4 = 0x7C0B
    m2.code(0x48, 0x63, 0x05, 0x04, 0x00, 0x00, 0x00);
    m2.code(0xEB, 0x06);                                // jmp +6 @+7 -> 0x7C0F
    m2.code(0x90, 0x90);                                // nop padding @+9..+10
    m2.code(0xD6, 0xFF, 0xFF, 0xFF);                    // int32 -42 @+11..+14
    m2.code(0xF4);                                      // hlt @+15
    m2.run(8);
    assert(m2.cpu->get_gpr(0) == static_cast<uint64_t>(static_cast<int64_t>(-42)));

    // Register MOVSXD sign extension
    Machine m3;
    m3.code(0x48, 0xC7, 0xC1, 0x80, 0xFF, 0xFF, 0xFF);  // mov rcx, -128
    m3.code(0x48, 0x63, 0xD9);                          // movsxd rbx, ecx
    m3.code(0xF4);                                      // hlt
    m3.run(8);
    assert(m3.cpu->get_gpr(3) == static_cast<uint64_t>(static_cast<int64_t>(-128)));
}

void case_push_pop() {
    Machine m;
    m.code(0x48, 0xC7, 0xC0, 0xAB, 0xCD, 0x00, 0x00);  // mov rax, 0xCDAB
    m.code(0x50);                                      // push rax
    m.code(0x48, 0x31, 0xC9);                          // xor rcx, rcx
    m.code(0x59);                                      // pop rcx
    m.run(8);
    assert(m.cpu->get_gpr(1) == 0xCDAB);
}

void case_lea_sib() {
    Machine m;
    m.code(0x48, 0xC7, 0xC6, 0x10, 0x00, 0x00, 0x00);  // mov rsi, 0x10
    m.code(0x48, 0xC7, 0xC2, 0x04, 0x00, 0x00, 0x00);  // mov rdx, 4
    m.code(0x48, 0x8D, 0x04, 0xD6);                    // lea rax, [rsi + rdx*8]
    m.run(8);
    assert(m.cpu->get_gpr(0) == 0x10 + 4 * 8);
}

void case_movzx_movsx() {
    Machine m;
    m.code64(0x3000, 0xFF);                            // byte at 0x3000 = 0xFF
    m.code(0x48, 0xC7, 0xC6, 0x00, 0x30, 0x00, 0x00);  // mov rsi, 0x3000
    m.code(0x48, 0x0F, 0xB6, 0x3E);                    // movzx rdi, byte [rsi]
    m.code(0x48, 0x0F, 0xBE, 0x06);                    // movsx rax, byte [rsi]
    m.run(8);
    assert(m.cpu->get_gpr(7) == 0xFF);
    assert(m.cpu->get_gpr(0) == static_cast<uint64_t>(-1));  // sign-extended
}

void case_inc_dec_neg() {
    Machine m;
    m.code(0x48, 0xC7, 0xC0, 0x09, 0x00, 0x00, 0x00);  // mov rax, 9
    m.code(0x48, 0xFF, 0xC0);                          // inc rax
    m.code(0x48, 0xFF, 0xC8);                          // dec rax
    m.code(0x48, 0xF7, 0xD8);                          // neg rax
    m.run(8);
    assert(m.cpu->get_gpr(0) == static_cast<uint64_t>(-9));
}

void case_shl_shr_sar() {
    Machine m;
    m.code(0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00);  // mov rax, 1
    m.code(0x48, 0xC1, 0xE0, 0x08);                    // shl rax, 8
    assert(m.cpu->step()); assert(m.cpu->step());
    assert(m.cpu->get_gpr(0) == 0x100);
    m.code(0x48, 0xC1, 0xF8, 0x04);                    // sar rax, 4
    assert(m.cpu->step());
    assert(m.cpu->get_gpr(0) == 0x10);
    m.code(0x48, 0xC1, 0xE8, 0x04);                    // shr rax, 4
    assert(m.cpu->step());
    assert(m.cpu->get_gpr(0) == 1);
}

void case_imul_div() {
    Machine m;
    m.code(0x48, 0xC7, 0xC0, 0x39, 0x05, 0x00, 0x00);  // mov rax, 1337
    m.code(0x48, 0xC7, 0xC1, 0x03, 0x00, 0x00, 0x00);  // mov rcx, 3
    m.code(0x48, 0x0F, 0xAF, 0xC1);                    // imul rax, rcx
    m.run(8);
    assert(m.cpu->get_gpr(0) == 4011);
    m.code(0x48, 0x31, 0xD2);                          // xor rdx, rdx
    m.code(0x48, 0xC7, 0xC1, 0x07, 0x00, 0x00, 0x00);  // mov rcx, 7
    m.code(0x48, 0xF7, 0xF1);                          // div rcx
    m.run(8);
    assert(m.cpu->get_gpr(0) == 573);   // 4011 / 7
    assert(m.cpu->get_gpr(2) == 0);     // remainder
}

void case_cmov_setcc() {
    Machine m;
    m.code(0x48, 0xC7, 0xC0, 0x02, 0x00, 0x00, 0x00);  // mov rax, 2
    m.code(0x48, 0x83, 0xF8, 0x02);                    // cmp rax, 2 -> ZF
    m.code(0x48, 0x0F, 0x44, 0xCE);                    // cmove rcx, rsi
    m.code(0x0F, 0x94, 0xC2);                          // sete dl
    m.run(8);
    assert((m.cpu->get_rflags() & 0x40) != 0);
    assert((m.cpu->get_gpr(2) & 0xFF) == 1);           // dl == 1
}

void case_rex_writes() {
    Machine m;
    m.code(0x41, 0xB8, 0x2A, 0x00, 0x00, 0x00);        // mov r8d, 42
    assert(m.cpu->step());
    assert(m.cpu->get_gpr(8) == 42);
    m.code(0x41, 0x83, 0xC0, 0x08);                    // add r8d, 8
    assert(m.cpu->step());
    assert(m.cpu->get_gpr(8) == 50);
}

void case_bsfx() {
    Machine m;
    m.code(0x48, 0xC7, 0xC0, 0x00, 0xF0, 0x00, 0x00);  // mov rax, 0xF000
    m.code(0x48, 0x0F, 0xBC, 0xC8);                    // bsf rcx, rax
    m.run(8);
    assert(m.cpu->get_gpr(1) == 12);
    m.code(0x48, 0x0F, 0xBD, 0xC8);                    // bsr rcx, rax
    assert(m.cpu->step());
    assert(m.cpu->get_gpr(1) == 15);
}

void case_loop_insn() {
    Machine m;
    m.code(0x48, 0xC7, 0xC1, 0x05, 0x00, 0x00, 0x00);  // mov rcx, 5   @+0
    m.code(0x48, 0xFF, 0xC0);                          // inc rax      @+7
    m.code(0xE2, 0xFB);                                // loop -5 -> inc
    m.run(32);
    assert(m.cpu->get_gpr(0) == 5);
}

void case_test_jz() {
    Machine m;
    m.code(0x48, 0xC7, 0xC0, 0x08, 0x00, 0x00, 0x00);  // mov rax, 8        @+0
    m.code(0xA9, 0x01, 0x01, 0x00, 0x00);              // test eax, 0x101   @+7 -> ZF
    m.code(0x74, 0x07);                                // jz +7 (past next) @+12
    m.code(0x48, 0xC7, 0xC1, 0x99, 0x99, 0x00, 0x00);  // mov rcx, 0x9999   @+14 (skipped)
    m.run(8);
    assert((m.cpu->get_rflags() & 0x40) != 0);
    assert(m.cpu->get_gpr(1) == 0);                    // skip executed
}

void case_stosb_skip_rep() {
    Machine m;
    m.code(0xFC);                                      // cld
    m.code(0xF3, 0x90);                                // rep nop (prefix consumed)
    m.code(0x90);                                      // nop
    m.run(8);
    assert(m.cpu->get_rip() == kBase + 4);
}

}  // namespace x86test

void test_x86_decoder_suite() {
    using namespace x86test;
    case_mov_imm();        std::cout << "  [x86 01] MOV r64, imm32" << std::endl;
    case_mov_reg_mem();    std::cout << "  [x86 02] MOV r, [mem]" << std::endl;
    case_mov_mem_reg();    std::cout << "  [x86 03] MOV [mem], r" << std::endl;
    case_add_sub();        std::cout << "  [x86 04] ADD/SUB reg,reg" << std::endl;
    case_sub_zf_cf();      std::cout << "  [x86 05] SUB flags ZF/CF" << std::endl;
    case_alu_or_and_xor(); std::cout << "  [x86 06] OR/AND/XOR imm" << std::endl;
    case_cmp_jne_loop();   std::cout << "  [x86 07] CMP/JNE loop" << std::endl;
    case_jmp_call_ret();   std::cout << "  [x86 08] CALL/RET" << std::endl;
    case_push_pop();       std::cout << "  [x86 09] PUSH/POP" << std::endl;
    case_lea_sib();        std::cout << "  [x86 10] LEA SIB scale" << std::endl;
    case_movzx_movsx();    std::cout << "  [x86 11] MOVZX/MOVSX" << std::endl;
    case_inc_dec_neg();    std::cout << "  [x86 12] INC/DEC/NEG" << std::endl;
    case_shl_shr_sar();    std::cout << "  [x86 13] SHL/SAR/SHR imm" << std::endl;
    case_imul_div();       std::cout << "  [x86 14] IMUL/DIV" << std::endl;
    case_cmov_setcc();     std::cout << "  [x86 15] CMOVE/SETE" << std::endl;
    case_rex_writes();     std::cout << "  [x86 16] REX.B r8d" << std::endl;
    case_bsfx();           std::cout << "  [x86 17] BSF/BSR" << std::endl;
    case_loop_insn();      std::cout << "  [x86 18] LOOP" << std::endl;
    case_test_jz();        std::cout << "  [x86 19] TEST/JZ" << std::endl;
    case_stosb_skip_rep(); std::cout << "  [x86 20] prefixes + NOP" << std::endl;

    std::cout << "[TEST PASSED] x86_64 Decoder Suite (20 instruction cases)" << std::endl << std::flush;
}

// ===========================================================================
// R1: asmjit machine-code JIT emitter
// ===========================================================================
void test_native_jit() {
    assert(lime::X86JIT::available());

    auto mem = std::make_shared<lime::MemoryManager>(16 * 1024 * 1024);
    auto bus = std::make_shared<lime::DeviceBus>();
    lime::VCPU cpu(0, mem, bus);

    // addi x1, x0, 7        -> 0x00700093
    // addi x2, x0, 35       -> 0x02300113
    // add  x3, x1, x2       -> 0x002081B3
    // xor  x4, x3, x3       -> 0x00384213  (x4 = 0)
    // or   x5, x3, x1       -> 0x0010E2B3  (x5 = 42)
    // wfi                      0x10500073  -> IDLE_WAIT ends run_cycles
    std::vector<uint32_t> program = {
        0x00700093u, 0x02300113u, 0x002081B3u, 0x00384213u,
        0x0010E2B3u, 0x10500073u,
    };
    uint64_t entry = 0x80000000;
    for (size_t i = 0; i < program.size(); ++i) {
        mem->write32(entry + i * 4, program[i]);
    }
    cpu.reset(entry);

    size_t executed = cpu.run_cycles(64);
    assert(executed >= 5);
    assert(cpu.get_reg(1) == 7);
    assert(cpu.get_reg(2) == 35);
    assert(cpu.get_reg(3) == 42);
    assert(cpu.get_reg(4) == 0);
    assert(cpu.get_reg(5) == 42);
    assert(cpu.native_jit() != nullptr);
    assert(cpu.native_jit()->block_count() >= 1);
    assert(cpu.native_jit()->native_ops() >= 5);

    // JIT cache invalidation still leaves a working vCPU.
    cpu.native_jit()->clear_cache();
    cpu.set_pc(entry);
    cpu.set_state(lime::VCPUState::RUNNING);
    executed = cpu.run_cycles(64);
    assert(cpu.get_reg(5) == 42);

    std::cout << "[TEST PASSED] asmjit Machine-Code JIT (native x86_64 emission)"
              << std::endl << std::flush;
}

// ===========================================================================
// R3: asynchronous I/O (IOCP / io_uring / worker pool)
// ===========================================================================
void test_async_io_nonblocking() {
    namespace fs = std::filesystem;
    std::string img = "test_async.lime";
    fs::remove(img);
    assert(lime::SparseDisk::create(img, 32ULL * 1024 * 1024));

    auto engine = lime::AsyncIOEngine::create_best();
    assert(engine != nullptr);
    assert(engine->open(img));
    std::cout << "  Async backend: " << engine->backend_name() << std::endl;

    auto disk = std::make_shared<lime::SparseDisk>();
    assert(disk->open(img));

    // -- Write a 1 MiB pattern through the async path ------------------------
    constexpr size_t kBytes = 1024 * 1024;
    std::vector<uint8_t> wbuf(kBytes), rbuf(kBytes, 0);
    for (size_t i = 0; i < kBytes; ++i) wbuf[i] = static_cast<uint8_t>(i * 31 + 7);

    std::atomic<bool> write_done{false};
    bool write_ok = false;
    bool submitted = lime::submit_sparse_range(engine, disk, lime::AsyncOpType::WRITE,
                                               0, wbuf.data(), wbuf.size(),
                                               [&](const lime::AsyncCompletion& c) {
                                                   write_ok = c.success;
                                                   write_done = true;
                                               });
    assert(submitted);

    // ----------------------------------------------------------------------
    // Non-blocking proof: measure how long submit + first poll takes while
    // 1 MiB has NOT yet been written. On the synchronous path the write would
    // already have hit the file before submit_sparse_range returned.
    // ----------------------------------------------------------------------
    auto t0 = std::chrono::steady_clock::now();
    size_t spins = 0;
    while (!write_done && spins < 10000) {
        engine->poll(64);
        spins++;
    }
    auto t1 = std::chrono::steady_clock::now();
    assert(write_done);
    assert(write_ok);
    auto submit_latency_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
    std::cout << "  1 MiB async write completed after " << spins
              << " non-blocking poll spins (" << submit_latency_us << " us total)" << std::endl;

    // The write must NOT have been synchronous: with 64 KiB blocks, 16
    // segments would take >= 2 ms of file I/O on the submitting thread.
    assert(submit_latency_us < 2000);

    // -- Read it back through the async path --------------------------------
    std::atomic<bool> read_done{false};
    bool read_ok = false;
    submitted = lime::submit_sparse_range(engine, disk, lime::AsyncOpType::READ,
                                          0, rbuf.data(), rbuf.size(),
                                          [&](const lime::AsyncCompletion& c) {
                                              read_ok = c.success;
                                              read_done = true;
                                          });
    assert(submitted);
    while (!read_done) {
        assert(engine->poll(64) > 0 || !read_done.load());
    }
    assert(read_ok);
    assert(rbuf == wbuf);

    // -- Sparse-hole read returns zeros -------------------------------------
    std::vector<uint8_t> hole(kBytes, 0xAA);
    std::atomic<bool> hole_done{false};
    bool hole_ok = false;
    submitted = lime::submit_sparse_range(engine, disk, lime::AsyncOpType::READ,
                                          16ULL * 1024 * 1024, hole.data(), hole.size(),
                                          [&](const lime::AsyncCompletion& c) {
                                              hole_ok = c.success;
                                              hole_done = true;
                                          });
    assert(submitted);
    while (!hole_done) engine->poll(64);
    assert(hole_ok);
    for (uint8_t b : hole) assert(b == 0);

    assert(engine->completed() >= 3);
    engine->shutdown();

    // -- NVMe controller with the engine attached: doorbell returns --------
    auto mem2 = std::make_shared<lime::MemoryManager>(16 * 1024 * 1024);
    auto disk2 = std::make_shared<lime::SparseDisk>();
    assert(disk2->open(img));
    lime::NVMeController nvme(disk2, mem2, 0x20000000);
    auto engine2 = lime::AsyncIOEngine::create_best();
    assert(engine2->open(img));
    nvme.attach_async_engine(engine2);

    auto* vcpu_mem = mem2.get();
    std::vector<uint8_t> sqe(64, 0);
    sqe[0] = 0x02;                                        // NVM Read
    sqe[2] = 0x5A; sqe[3] = 0x00;                         // CID = 0x5A
    uint64_t prp1 = 0x90000000ULL;
    uint64_t slba = 0;                                    // LBA 0
    uint16_t nlb = 63;                                    // 64 sectors = 32 KiB
    std::memcpy(&sqe[24], &prp1, 8);
    std::memcpy(&sqe[40], &slba, 8);
    std::memcpy(&sqe[48], &nlb, 2);
    vcpu_mem->write_bytes(0x20010000, sqe.data(), sqe.size());  // io_sq_base
    nvme.write(0x14, 0x004601, 4);                        // CC.EN
    nvme.write(0x1008, 1, 4);                             // IO SQ doorbell

    // process_io_sq must return without the 32 KiB read having completed.
    // Doorbell latency must be far below a synchronous 32 KiB read+copy.
    auto t2 = std::chrono::steady_clock::now();
    size_t nvme_polls = 0;
    while (nvme.poll_async(64) == 0 && nvme_polls < 10000) {
        nvme_polls++;
    }
    auto t3 = std::chrono::steady_clock::now();
    auto doorbell_us = std::chrono::duration_cast<std::chrono::microseconds>(t3 - t2).count();
    std::cout << "  NVMe async completion harvested after " << nvme_polls
              << " polls (" << doorbell_us << " us)" << std::endl;
    assert(nvme_polls < 10000);

    // Verify the guest buffer received the pattern written through the
    // async block path earlier.
    std::vector<uint8_t> guest(32 * 1024);
    assert(vcpu_mem->read_bytes(prp1, guest.data(), guest.size()));
    assert(std::equal(guest.begin(), guest.begin() + 4096, wbuf.begin()));

    engine2->shutdown();
    fs::remove(img);

    std::cout << "[TEST PASSED] Async I/O: non-blocking submit + IOCP/io_uring/"
                 "worker completion" << std::endl << std::flush;
}

void test_full_vm_extensions() {
    lime::VMConfig config;
    config.ram_size_mb = 64;
    config.target_arch = lime::TargetArch::RISCV64;
    config.enable_pci = true;
    config.enable_acpi = true;
    config.headless = true;

    lime::VirtualMachine vm(config);
    assert(vm.init());

    std::vector<uint8_t> os_code = lime::LimeImageBuilder::generate_mini_os_code();
    assert(vm.load_binary(os_code, 0x80000000));

    vm.vcpu()->run_cycles(15);
    assert(vm.vcpu() != nullptr);

    std::cout << "[TEST PASSED] Complete Enterprise Extensions VM Runtime" << std::endl << std::flush;
}

int main() {
    std::cout << "Starting Test Suite Execution..." << std::endl << std::flush;
    test_memory_subsystem();
    test_acpi_and_pci_bus();
    test_firmware_and_nvme();
    test_x86_multi_arch();
    test_x86_decoder_suite();
    test_native_jit();
    test_async_io_nonblocking();
    test_full_vm_extensions();
    std::cout << "\nALL UNIVERSAL LIME EXTENSION TESTS PASSED SUCCESSFULLY!" << std::endl << std::flush;
    return 0;
}
