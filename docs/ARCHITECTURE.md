# LIME Architecture and Support Status

LIME is an experimental C++20 software emulator. Its default execution path is a RISC-V interpreter with an optional AsmJit translator for selected basic-block operations. The x86 decoder is a separate, partial software implementation. WHPX and KVM are declared in the API but are not operational backends.

## Runtime composition

`src/main.cpp` enters the CLI dispatcher in `src/cli.cpp`. The `run` command initializes `VirtualMachine`, loads a `.lime` sparse image, and runs either the RISC-V vCPU threads or the x86 decoder. VM-owned memory, devices, storage, scheduler, and host I/O services are assembled in `src/vm.cpp`.

The RISC-V path fetches and executes guest instructions through `VCPU`. `JITEngine` groups instructions into basic blocks; `X86JIT` emits host x86-64 code for selected integer operations and calls the interpreter for operations it does not inline. Without AsmJit or when a block cannot be compiled, the interpreter remains the fallback.

The x86 decoder executes a limited integer instruction subset in a flat memory model. The VM does not currently provide a complete x86 boot flow or privilege-transition implementation.

## Memory

`MemoryManager` stores guest pages sparsely and limits the number of committed pages to the configured capacity. Guest physical addresses are sparse and may be above zero; the capacity is a commitment limit, not a contiguous GPA range. Reads from unallocated pages create zero-filled pages. Out-of-memory accesses return zero or fail the byte-range API; CPU/device paths do not yet expose a uniform guest fault model.

The balloon implementation discards clean pages and can restore them as zeroes. Dirty pages are retained because there is no swap or backing store. Consequently, automatic idle reclamation is limited and does not currently reclaim modified guest RAM.

`MMU` includes an Sv39 walker and a separate x86 page-table walker. The x86 decoder does not currently use the x86 walker as part of a complete execution pipeline.

## Storage and devices

`.lime` images contain a packed versioned header, a block allocation table, and lazily allocated 64 KiB blocks. `BlockStorage` is the sector interface used by VirtIO block, NVMe, sparse disks, and CoW overlays. A CoW write copies the affected base block into the delta on first write, then changes requested sectors; this preserves untouched sectors and permits zero-filled overrides.

Async file I/O backends include IOCP on Windows, io_uring when available on Linux, and a worker-thread fallback. The current sparse-range async path operates on `SparseDisk`; CoW traffic uses the shared synchronous sector interface. NVMe and VirtIO queue protocols are simplified and should not be treated as full device-spec implementations.

The MMIO bus includes UART, VirtIO-style block/network/balloon/graphics/input devices, CLINT, PLIC, and optional NVMe/PCI. Several additional classes in the source tree (USB, TPM, audio, HPET, ACPI extensions, filesystem, and UEFI shell) are scaffolding or independent components and are not all registered by VM initialization.

## Startup and platform limits

VM initialization validates basic configuration and propagates configured firmware, kernel, image, and network bridge failures. A missing sparse disk path can create a new 10 GiB sparse image; an existing invalid image is rejected rather than overwritten. CoW requires an existing base image and creates a missing delta image. Guest OS boot compatibility remains experimental.

The `HostHypervisor` methods currently report unsupported/failure. LIME does not run guests through WHPX or KVM. The CLI rejects `--hardware-vt` until that execution path exists.

## Build and verification

`build.sh` builds the CLI and `tests/test_lime.cpp`, including the vendored AsmJit sources. The release workflow builds platform binaries when a GitHub Release is created. The test suite covers selected memory, storage, CoW, MMIO, x86 decode, JIT, async I/O, and VM initialization behavior; it does not establish compatibility with production operating systems.
