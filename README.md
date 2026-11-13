<p align="center">
  <img src="assets/lime.png" alt="LIME Logo" width="300"/>
</p>

# LIME: Lightweight Userspace Virtual Machine Runtime

LIME is an experimental C++20 virtual machine emulator. It currently provides a RISC-V interpreter with an optional x86-64 host-code JIT, a separate partial x86 software decoder, guest memory and MMIO devices, and sparse disk images. It is not yet a production hypervisor or a general-purpose operating-system boot environment.

The project focuses on making the emulated machine small and inspectable. See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for implemented behavior and current limits.

---

## Key Features

- **Execution:** RISC-V interpreter and an optional AsmJit-based x86-64 host-code JIT for selected RISC-V operations. x86 guest decoding is a separate partial software implementation.
- **Memory:** Sparse page allocation with a commitment limit, a basic balloon device, and Sv39 translation code. Dirty guest pages are not reclaimed because there is no swap backing store yet.

- **Storage:** `.lime` sparse images with 64 KiB blocks and CoW overlays. VirtIO block and the simplified NVMe device share a sector-storage interface; CoW performs block copy-up on first write. Async file I/O currently applies only to sparse base images.

- **Devices:** Basic UART, VirtIO-style MMIO devices, CLINT/PLIC, PCI ECAM, and simplified NVMe are implemented to varying levels. Device queue handling and guest OS compatibility are incomplete.
- **Host virtualization:** WHPX/KVM execution is not implemented. `--hardware-vt` is rejected rather than silently falling back.

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

### 2. Build from Source

Use a C++20 compiler and Bash. The build script compiles the vendored AsmJit sources, then builds both the CLI and test executable:

```bash
bash build.sh
./test_lime       # Windows: ./test_lime.exe
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

The current CLI runs the software emulator. Firmware and kernel-loading support is experimental; image validation and configured file loading failures are reported.

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
