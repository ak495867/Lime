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
#include <iostream>
#include <cassert>
#include <vector>
#include <string>

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
    test_full_vm_extensions();
    std::cout << "\nALL UNIVERSAL LIME EXTENSION TESTS PASSED SUCCESSFULLY!" << std::endl << std::flush;
    return 0;
}
