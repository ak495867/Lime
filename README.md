<p align="center">
  <img src="assets/lime.png" alt="LIME Logo" width="300"/>
</p>

# LIME: Lightweight Userspace Virtual Machine Runtime

LIME is a high-performance, lightweight C++ virtual machine and hardware abstraction layer designed to execute miniature operating systems, micro-kernels, security sandboxes, and embedded workloads inside a host OS while consuming dramatically fewer resources than traditional hypervisors like QEMU.

The defining principle of LIME is **dynamic compute allocation**: give the virtual machine exactly what it needs, when it needs it, and aggressively reclaim host resources the moment the guest goes idle.

---

## Key Features

- **Dynamic Compute & Memory Allocation:**
  - **Demand Paging:** Guest RAM is allocated only when physical pages are touched for the first time.
  - **Memory Ballooning:** Guest or host policy can inflate/deflate memory balloon pages.
  - **Idle Page Reclamation:** Background worker trims untouched RAM pages back to the host system.
  - **Zero-Footprint Idle Sleep:** vCPU `WFI` (Wait For Interrupt) detection dynamically sleeps host threads to reduce background CPU usage to near 0%.

- **Sparse Virtual Storage (`.lime` Format):**
  - High-performance 64 KB block allocation map. A guest configured with a 100 MB virtual drive consumes ~70 KB on physical host storage until write operations occur.
  - **Copy-On-Write (CoW) Delta Overlay:** Instant 0-cost VM disk cloning by chaining read-only base images with read-write overlay layers.

- **Universal Hardware & Device Subsystems:**
  - **MMU Paging:** Sv39 3-level page table translation (`VPN2`, `VPN1`, `VPN0`) for Virtual Address to Guest Physical Address mapping.
  - **Interrupt Controllers:** Core Local Interruptor (**CLINT**) for hardware timer ticks and Platform-Level Interrupt Controller (**PLIC**) for external IRQs.
  - **PCI Express Bus (ECAM):** 256-bus configuration space (`0xE0000000`) supporting dynamic device probing.
  - **VirtIO Device Modules:** VirtIO-Block, VirtIO-Net (with host socket bridge), VirtIO-Balloon, VirtIO-GPU, and 16550 UART Serial Console.
  - **NVMe Controller Subsystem:** PCI Express mass storage driver with Admin and I/O submission queues.
  - **ACPI Table Engine:** Generates RSDP, XSDT, and MADT (APIC) tables for Linux and OS kernel booting.
  - **Firmware Loader:** Maps UEFI (OVMF) or SeaBIOS ROM images into guest high memory.

- **Multi-Architecture Execution Engine:**
  - RISC-V 64-bit / 32-bit execution target with full privilege levels (Machine, Supervisor, User).
  - x86_64 target execution engine supporting Real Mode (16-bit), Protected Mode (32-bit), and Long Mode (64-bit).
  - Pluggable Host Native Virtualization abstraction (WHPX on Windows, KVM on Linux).

---

## Installation & Building

### 1. Install Pre-compiled Binaries (Recommended)

**Linux / macOS (via curl):**
```bash
curl -fsSL https://github.com/ak495867/Lime/raw/main/install.sh | bash
```

**Windows (via PowerShell):**
```powershell
irm https://github.com/ak495867/Lime/raw/main/install.ps1 | iex
```
*(This script will download `lime.exe` and configure your PATH so you can run `lime` directly from any command prompt).*

### 2. Manual Compilation from Source

LIME requires a standard C++20 compiler (`g++`, `clang++`, or MSVC).

**Compiling LIME CLI Executable:**
```powershell
g++ -std=c++20 -Iinclude \
    src/memory.cpp src/storage.cpp src/cow_disk.cpp src/devices.cpp \
    src/interrupts.cpp src/mmu.cpp src/hypervisor.cpp src/net_bridge.cpp \
    src/jit.cpp src/acpi.cpp src/pci.cpp src/firmware.cpp src/nvme.cpp \
    src/x86_cpu.cpp src/vcpu.cpp src/scheduler.cpp src/vm.cpp src/cli.cpp src/main.cpp \
    -o lime.exe -lws2_32 -lWinHvPlatform
```

**Compiling Test Suite:**
```powershell
g++ -std=c++20 -Iinclude \
    src/memory.cpp src/storage.cpp src/cow_disk.cpp src/devices.cpp \
    src/interrupts.cpp src/mmu.cpp src/hypervisor.cpp src/net_bridge.cpp \
    src/jit.cpp src/acpi.cpp src/pci.cpp src/firmware.cpp src/nvme.cpp \
    src/x86_cpu.cpp src/vcpu.cpp src/scheduler.cpp src/vm.cpp tests/test_lime.cpp \
    -o test_lime.exe -lws2_32 -lWinHvPlatform

.\test_lime.exe
```

---

## CLI Usage Guide

### 1. Build a Sparse Disk Image

Create a new 100 MB sparse virtual disk:

```bash
lime build image.lime --size 100M
```

### 2. Inspect Virtual Disk Structure

Display virtual capacity, actual host storage used, and allocated block counts:

```bash
lime inspect image.lime
```

### 3. Create a Copy-On-Write (CoW) Delta Overlay

Create a 0-cost snapshot layer on top of a base image:

```bash
lime overlay base.lime delta.lime
```

### 4. Boot a Virtual Machine

Boot a guest image with 256 MB RAM under aggressive resource reclamation policy:

```bash
lime run image.lime --ram 256M --cpu auto --policy aggressive --net
```

Boot with native host hardware virtualization enabled:

```bash
lime run image.lime --ram 512M --hardware-vt
```

---

## Project Structure

```text
d:\Lime
├── include/lime/           # C++ Header declarations
│   ├── acpi.hpp            # ACPI RSDP/MADT/FADT table generator
│   ├── cli.hpp             # Command Line Interface dispatcher
│   ├── cow_disk.hpp        # Copy-On-Write sparse disk overlay
│   ├── devices.hpp         # VirtIO & UART MMIO virtual devices
│   ├── firmware.hpp       # UEFI OVMF & SeaBIOS firmware loader
│   ├── hypervisor.hpp     # Host native virtualization abstraction (WHPX/KVM)
│   ├── interrupts.hpp     # CLINT timer & PLIC interrupt controllers
│   ├── jit.hpp            # Basic block JIT translation cache
│   ├── memory.hpp         # Dynamic demand-paged memory manager & ballooning
│   ├── mmu.hpp            # Sv39 3-level MMU page table translation
│   ├── net_bridge.hpp     # Host socket packet bridge
│   ├── nvme.hpp           # NVMe storage controller
│   ├── pci.hpp            # PCI Express bus & ECAM memory region
│   ├── scheduler.hpp      # Host resource scheduler & sleep loop
│   ├── storage.hpp        # Sparse virtual disk (.lime) driver
│   ├── vcpu.hpp           # RISC-V 64-bit vCPU execution engine
│   ├── vm.hpp             # VirtualMachine orchestrator
│   └── x86_cpu.hpp        # x86_64 target CPU decoder
├── src/                    # C++ Source implementations
├── tests/                  # Integration & unit test runner (test_lime.cpp)
├── LICENSE                 # MIT License (ak495867)
└── README.md               # Documentation
```

---

## License

Copyright (c) 2026 **ak495867**. Licensed under the [MIT License](LICENSE).
