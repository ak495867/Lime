#include <cstdint>
#include <iostream>
#include <vector>
#include <cassert>
#include "lime/memory.hpp"
#include "lime/devices.hpp"
#include "lime/x86_cpu.hpp"

namespace {

constexpr uint64_t kBase = 0x7C00;
constexpr uint64_t kStackTop = 0x20000;

struct TestMachine {
    std::shared_ptr<lime::MemoryManager> mem;
    std::shared_ptr<lime::DeviceBus> bus;
    std::unique_ptr<lime::X86CPUDecoder> cpu;
    uint64_t cursor = kBase;

    TestMachine()
        : mem(std::make_shared<lime::MemoryManager>(4 * 1024 * 1024)),
          bus(std::make_shared<lime::DeviceBus>()) {
        cpu = std::make_unique<lime::X86CPUDecoder>(mem, bus);
        cpu->reset(kBase);
        cpu->set_gpr(4, kStackTop); // RSP = kStackTop (16-byte aligned)
    }

    template <typename... Bytes>
    void code(Bytes... bytes) {
        uint8_t buf[] = {static_cast<uint8_t>(bytes)...};
        for (uint8_t b : buf) mem->write8(cursor++, b);
    }

    void run(unsigned max_steps = 64) {
        for (unsigned i = 0; i < max_steps; ++i) {
            if (!cpu->step()) return;
        }
    }
};

} // namespace

int main() {
    std::cout << "=== Challenger 2 Empirical Test Harness ===" << std::endl;

    int passed = 0;
    int failed = 0;

    auto check = [&](const char* name, bool cond, const std::string& detail = "") {
        if (cond) {
            std::cout << "  [PASS] " << name << std::endl;
            passed++;
        } else {
            std::cout << "  [FAIL] " << name;
            if (!detail.empty()) std::cout << " -> " << detail;
            std::cout << std::endl;
            failed++;
        }
    };

    // -----------------------------------------------------------------------
    // Section 1: Stack Operations & RSP Alignment (PUSH, POP, RSP in 64-bit mode)
    // -----------------------------------------------------------------------
    std::cout << "\n--- Section 1: Stack Operations & RSP Alignment ---" << std::endl;

    // Test 1.1: PUSH r64 (opcode 0x50) should push 8 bytes and decrement RSP by 8
    {
        TestMachine m;
        // movabs rax, 0x1122334455667788
        m.code(0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11);
        m.code(0x50); // push rax (standard 1-byte opcode in 64-bit mode)
        m.code(0xF4); // hlt
        m.run(8);

        uint64_t rsp_after_push = m.cpu->get_gpr(4);
        check("1.1 PUSH r64 decrements RSP by 8",
              rsp_after_push == kStackTop - 8,
              "Expected RSP=" + std::to_string(kStackTop - 8) + " but got " + std::to_string(rsp_after_push));

        uint64_t pushed_val = m.mem->read64(rsp_after_push);
        check("1.1b PUSH r64 writes full 64-bit value to stack",
              pushed_val == 0x1122334455667788ULL,
              "Expected 0x1122334455667788 but got " + std::to_string(pushed_val));
    }

    // Test 1.2: POP r64 (opcode 0x58) should restore full 64-bit value and increment RSP by 8
    {
        TestMachine m;
        m.code(0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11); // movabs rax, ...
        m.code(0x50); // push rax
        m.code(0x48, 0x31, 0xC0); // xor rax, rax
        m.code(0x58); // pop rax
        m.code(0xF4);
        m.run(8);

        uint64_t rsp_after_pop = m.cpu->get_gpr(4);
        uint64_t rax_after_pop = m.cpu->get_gpr(0);
        check("1.2 POP r64 restores RSP to kStackTop",
              rsp_after_pop == kStackTop,
              "Expected RSP=" + std::to_string(kStackTop) + " but got " + std::to_string(rsp_after_pop));
        check("1.2b POP r64 restores 64-bit RAX",
              rax_after_pop == 0x1122334455667788ULL,
              "Expected 0x1122334455667788 but got " + std::to_string(rax_after_pop));
    }

    // Test 1.3: PUSH/POP with REX.B (r8..r15)
    {
        TestMachine m;
        m.code(0x49, 0xB8, 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22); // mov r8, ...
        m.code(0x41, 0x50); // push r8
        m.code(0x41, 0x59); // pop r9
        m.code(0xF4);
        m.run(8);

        uint64_t r9 = m.cpu->get_gpr(9);
        uint64_t rsp = m.cpu->get_gpr(4);
        check("1.3 PUSH/POP r8/r9 preserves 64-bit value",
              r9 == 0x2211FFEEDDCCBBAAULL,
              "Expected 0x2211FFEEDDCCBBAA but got " + std::to_string(r9));
        check("1.3b PUSH/POP r8/r9 maintains RSP",
              rsp == kStackTop,
              "Expected RSP=" + std::to_string(kStackTop) + " but got " + std::to_string(rsp));
    }

    // Test 1.4: RSP Alignment across nested PUSH/POP (16-byte alignment invariant)
    {
        TestMachine m;
        // Stack top is 0x20000 (0x20000 % 16 == 0)
        // push rax -> RSP = 0x1FFF8 (RSP % 16 == 8)
        // push rbx -> RSP = 0x1FFF0 (RSP % 16 == 0)
        m.code(0x50); // push rax
        m.code(0x53); // push rbx
        m.code(0x5B); // pop rbx
        m.code(0x58); // pop rax
        m.code(0xF4);

        m.cpu->step(); // push rax
        uint64_t rsp1 = m.cpu->get_gpr(4);
        m.cpu->step(); // push rbx
        uint64_t rsp2 = m.cpu->get_gpr(4);
        m.cpu->step(); // pop rbx
        uint64_t rsp3 = m.cpu->get_gpr(4);
        m.cpu->step(); // pop rax
        uint64_t rsp4 = m.cpu->get_gpr(4);

        check("1.4 1st PUSH sets RSP % 16 == 8", (rsp1 % 16) == 8, "rsp1=" + std::to_string(rsp1));
        check("1.4b 2nd PUSH sets RSP % 16 == 0", (rsp2 % 16) == 0, "rsp2=" + std::to_string(rsp2));
        check("1.4c Stack completely balanced after pops", rsp4 == kStackTop, "rsp4=" + std::to_string(rsp4));
    }

    // -----------------------------------------------------------------------
    // Section 2: Control Flow Semantics (CALL, RET, JMP, Jcc)
    // -----------------------------------------------------------------------
    std::cout << "\n--- Section 2: Control Flow Semantics ---" << std::endl;

    // Test 2.1: CALL rel32 decrements RSP by 8 in 64-bit mode and pushes 64-bit return address
    {
        TestMachine m;
        // call +5 (5 bytes: E8 05 00 00 00) -> lands at +10
        // +5: hlt (0xF4)
        // +6..+9: padding
        // +10: ret (0xC3)
        m.code(0xE8, 0x05, 0x00, 0x00, 0x00); // call rel32 +5
        m.code(0xF4);                         // hlt @+5 (target for ret)
        m.code(0x90, 0x90, 0x90, 0x90);       // padding @+6..+9
        m.code(0xC3);                         // ret @+10

        m.cpu->step(); // execute CALL
        uint64_t rsp_in_subroutine = m.cpu->get_gpr(4);
        uint64_t ret_addr_on_stack = m.mem->read64(rsp_in_subroutine);

        check("2.1 CALL rel32 decrements RSP by 8",
              rsp_in_subroutine == kStackTop - 8,
              "Expected RSP=" + std::to_string(kStackTop - 8) + " but got " + std::to_string(rsp_in_subroutine));
        check("2.1b CALL rel32 pushes exact 64-bit return address (kBase + 5)",
              ret_addr_on_stack == kBase + 5,
              "Expected return address " + std::to_string(kBase + 5) + " but got " + std::to_string(ret_addr_on_stack));

        m.cpu->step(); // execute RET
        check("2.2 RET restores RSP to kStackTop",
              m.cpu->get_gpr(4) == kStackTop,
              "Expected RSP=" + std::to_string(kStackTop) + " but got " + std::to_string(m.cpu->get_gpr(4)));
        check("2.2b RET jumps back to return address (kBase + 5)",
              m.cpu->get_rip() == kBase + 5,
              "Expected RIP=" + std::to_string(kBase + 5) + " but got " + std::to_string(m.cpu->get_rip()));
    }

    // Test 2.3: Indirect CALL r64 (FF /2) and JMP r64 (FF /4)
    {
        TestMachine m;
        // mov rax, kBase + 12 (0x7C0C)
        // jmp rax (FF E0)
        // +5..+11: padding / failure
        // +12: mov rbx, 123
        // +19: hlt
        m.code(0x48, 0xC7, 0xC0, 0x0C, 0x7C, 0x00, 0x00); // mov rax, 0x7C0C
        m.code(0xFF, 0xE0);                               // jmp rax
        m.code(0x90, 0x90, 0x90, 0x90, 0x90);
        m.code(0x48, 0xC7, 0xC3, 0x7B, 0x00, 0x00, 0x00); // mov rbx, 123 @+12
        m.code(0xF4);
        m.run(8);

        check("2.3 Indirect JMP rax reaches target",
              m.cpu->get_gpr(3) == 123,
              "RBX=" + std::to_string(m.cpu->get_gpr(3)));
    }

    // Test 2.4: Jcc conditional jumps exhaustive (all 16 conditions taken vs not-taken)
    {
        bool all_jcc_ok = true;
        // Test condition 4 (JZ / JE): taken when ZF=1, not taken when ZF=0
        {
            TestMachine m;
            // xor rax, rax -> ZF=1
            // jz +5 (74 05) -> skip 5 bytes
            // mov rbx, 1 (should be skipped)
            // mov rbx, 2 (target)
            m.code(0x48, 0x31, 0xC0);                         // xor rax, rax (3)
            m.code(0x74, 0x07);                               // jz +7 (2)
            m.code(0x48, 0xC7, 0xC3, 0x01, 0x00, 0x00, 0x00);// mov rbx, 1 (7)
            m.code(0x48, 0xC7, 0xC3, 0x02, 0x00, 0x00, 0x00);// mov rbx, 2 (7)
            m.code(0xF4);
            m.run(8);
            if (m.cpu->get_gpr(3) != 2) all_jcc_ok = false;
        }
        // Test condition 5 (JNZ / JNE): not taken when ZF=1
        {
            TestMachine m;
            m.code(0x48, 0x31, 0xC0);                         // xor rax, rax -> ZF=1 (3)
            m.code(0x75, 0x07);                               // jnz +7 (not taken) (2)
            m.code(0x48, 0xC7, 0xC3, 0x05, 0x00, 0x00, 0x00);// mov rbx, 5 (7)
            m.code(0xEB, 0x07);                               // jmp +7
            m.code(0x48, 0xC7, 0xC3, 0x06, 0x00, 0x00, 0x00);// mov rbx, 6
            m.code(0xF4);
            m.run(8);
            if (m.cpu->get_gpr(3) != 5) all_jcc_ok = false;
        }
        // Test 0F 8x (Jcc rel32)
        {
            TestMachine m;
            m.code(0x48, 0x31, 0xC0);                         // xor rax, rax (3)
            m.code(0x0F, 0x84, 0x07, 0x00, 0x00, 0x00);       // jz rel32 +7 (6)
            m.code(0x48, 0xC7, 0xC3, 0x11, 0x00, 0x00, 0x00);// mov rbx, 11 (7)
            m.code(0x48, 0xC7, 0xC3, 0x22, 0x00, 0x00, 0x00);// mov rbx, 22 (7)
            m.code(0xF4);
            m.run(8);
            if (m.cpu->get_gpr(3) != 22) all_jcc_ok = false;
        }
        check("2.4 Jcc rel8 and rel32 condition evaluation", all_jcc_ok);
    }

    // -----------------------------------------------------------------------
    // Section 3: Bit Manipulation and Shifts
    // -----------------------------------------------------------------------
    std::cout << "\n--- Section 3: Bit Manipulation & Shifts ---" << std::endl;

    // Test 3.1: 64-bit shift by 32 (SHL rax, 32)
    {
        TestMachine m;
        m.code(0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00); // mov rax, 1
        m.code(0x48, 0xC1, 0xE0, 0x20);                   // shl rax, 32 (0x20)
        m.code(0xF4);
        m.run(8);

        uint64_t rax = m.cpu->get_gpr(0);
        check("3.1 SHL r64, 32 shifts into upper 32 bits (1 << 32)",
              rax == (1ULL << 32),
              "Expected " + std::to_string(1ULL << 32) + " but got " + std::to_string(rax));
    }

    // Test 3.2: 64-bit shift with CL (SHL rax, cl where cl = 40)
    {
        TestMachine m;
        m.code(0x48, 0xC7, 0xC0, 0x01, 0x00, 0x00, 0x00); // mov rax, 1
        m.code(0x48, 0xC7, 0xC1, 0x28, 0x00, 0x00, 0x00); // mov rcx, 40
        m.code(0x48, 0xD3, 0xE0);                         // shl rax, cl
        m.code(0xF4);
        m.run(8);

        uint64_t rax = m.cpu->get_gpr(0);
        check("3.2 SHL r64, CL where CL=40 (1 << 40)",
              rax == (1ULL << 40),
              "Expected " + std::to_string(1ULL << 40) + " but got " + std::to_string(rax));
    }

    // Test 3.3: 64-bit SHR by 32
    {
        TestMachine m;
        m.code(0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12); // movabs rax, 0x1234567800000000
        m.code(0x48, 0xC1, 0xE8, 0x20);                                     // shr rax, 32
        m.code(0xF4);
        m.run(8);

        uint64_t rax = m.cpu->get_gpr(0);
        check("3.3 SHR r64, 32 shifts right across 32-bit boundary",
              rax == 0x12345678ULL,
              "Expected 0x12345678 but got " + std::to_string(rax));
    }

    // Test 3.4: 64-bit SAR by 32 (sign extension)
    {
        TestMachine m;
        m.code(0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80); // movabs rax, 0x8000000000000000
        m.code(0x48, 0xC1, 0xF8, 0x20);                                     // sar rax, 32
        m.code(0xF4);
        m.run(8);

        uint64_t rax = m.cpu->get_gpr(0);
        check("3.4 SAR r64, 32 arithmetic sign extends negative value",
              rax == 0xFFFFFFFF80000000ULL,
              "Expected 0xFFFFFFFF80000000 but got " + std::to_string(rax));
    }

    // Test 3.5: BT (Bit Test) on register: BT rax, rcx (0F A3 C8)
    {
        TestMachine m;
        // mov rax, 8 (bit 3 set)
        // mov rcx, 3 (test bit 3)
        // bt rax, rcx (0F A3 C8) -> CF should be 1
        m.code(0x48, 0xC7, 0xC0, 0x08, 0x00, 0x00, 0x00); // mov rax, 8
        m.code(0x48, 0xC7, 0xC1, 0x03, 0x00, 0x00, 0x00); // mov rcx, 3
        m.code(0x48, 0x0F, 0xA3, 0xC8);                   // bt rax, rcx (modrm 0xC8 = reg rax, rcx)
        m.code(0xF4);
        m.run(8);

        bool cf = (m.cpu->get_rflags() & 0x01) != 0;
        check("3.5 BT r64, r64 sets CF=1 when bit is set",
              cf,
              "Expected CF=1 but got CF=" + std::to_string(cf));
    }

    // Test 3.6: BTS (Bit Test and Set) on register: BTS rax, rcx (0F AB C8)
    {
        TestMachine m;
        // mov rax, 0
        // mov rcx, 4
        // bts rax, rcx -> rax should become 0x10 (16), CF should be 0
        m.code(0x48, 0x31, 0xC0);                         // xor rax, rax
        m.code(0x48, 0xC7, 0xC1, 0x04, 0x00, 0x00, 0x00);// mov rcx, 4
        m.code(0x48, 0x0F, 0xAB, 0xC8);                   // bts rax, rcx
        m.code(0xF4);
        m.run(8);

        uint64_t rax = m.cpu->get_gpr(0);
        bool cf = (m.cpu->get_rflags() & 0x01) != 0;
        check("3.6 BTS r64, r64 sets bit 4 in RAX",
              rax == 16,
              "Expected RAX=16 but got " + std::to_string(rax));
        check("3.6b BTS r64, r64 sets CF=0 (previous bit value)",
              !cf,
              "Expected CF=0 but got CF=" + std::to_string(cf));
    }

    std::cout << "\n==============================================" << std::endl;
    std::cout << "Results: " << passed << " passed, " << failed << " failed." << std::endl;
    return failed > 0 ? 1 : 0;
}
