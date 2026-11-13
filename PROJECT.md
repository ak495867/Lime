# LIME Project Status

LIME is an experimental software emulator in C++20. The supported core is the RISC-V interpreter with an optional AsmJit translator for a selected instruction subset. The x86 decoder and hardware/device models are partial. WHPX/KVM host execution is not implemented.

## Current priorities

1. Keep guest storage correct through the shared `BlockStorage` contract, including CoW copy-up and explicit I/O errors.
2. Keep guest memory state safe. Modified pages remain resident until a real backing-store design exists.
3. Validate configuration and report firmware, disk, and device initialization failures.
4. Define supported instruction and device behavior with focused tests.
5. Keep README and architecture docs aligned with tested implementation.

## Support boundaries

- `SparseDisk` and `CoWSparseDisk` implement sector storage. Async sparse-range I/O is currently available only for `SparseDisk`.
- VirtIO and NVMe are simplified device models; queue protocol coverage is incomplete.
- x86 execution is a partial software decoder and not a general boot implementation.
- `HostHypervisor` is a placeholder API, not a WHPX/KVM backend.
- Firmware/kernel loading is experimental and does not guarantee OS boot compatibility.

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) for the current runtime path and limits.
