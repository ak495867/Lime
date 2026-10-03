# LIME Technical Architecture Specification

This document provides a deep architectural overview of the **LIME Virtual Machine Runtime**.

---

## Architecture Diagram

```
+-------------------------------------------------------------------+
|                           CLI Runtime                             |
|        (lime run, lime build, lime overlay, lime inspect)         |
+-------------------------------------------------------------------+
                                  |
                                  v
+-------------------------------------------------------------------+
|                        VirtualMachine Engine                      |
+-------------------------------------------------------------------+
     |              |               |               |            |
     v              v               v               v            v
+----------+  +-----------+  +-------------+  +-----------+ +--------+
|   vCPU   |  |   Memory  |  | Virtual Bus |  | Scheduler | |  CoW   |
| Engine   |  | Manager   |  |  (MMIO/PCI) |  | & Sleep   | | Storage|
+----------+  +-----------+  +-------------+  +-----------+ +--------+
     |              |               |               |            |
     |              |       +-------+-------+       |            |
     v              v       v       v       v       v            v
+----------+  +-------+  +----+ +------+ +----+ +-------+    +-------+
| MMU Sv39 |  |Paging |  |UART| |VirtIO| |NVMe| |Reclaim|    |Sparse |
| Paging   |  |Balloon|  |    | |Block | |    | |Loop   |    |Disk   |
+----------+  +-------+  +----+ +------+ +----+ +-------+    +-------+
```

---

## 1. Virtual Memory Subsystem (`MemoryManager` & `MMU`)

The memory subsystem is built on **demand paging** and **dynamic commitment**.

### Physical Address Layout
- **`0x00000000 - 0x000EFFFF`**: Low memory scratchpad & BIOS vector area.
- **`0x000F0000 - 0x000FFFFF`**: ACPI RSDP, XSDT, and MADT tables.
- **`0x02000000 - 0x0200FFFF`**: Core Local Interruptor (**CLINT**) hardware registers.
- **`0x0C000000 - 0x0FFFFFFF`**: Platform-Level Interrupt Controller (**PLIC**) registers.
- **`0x10000000 - 0x100000FF`**: 16550 UART Serial Console MMIO.
- **`0x10001000 - 0x10001FFF`**: VirtIO-Block MMIO.
- **`0x10002000 - 0x10002FFF`**: VirtIO-Net MMIO.
- **`0x10003000 - 0x10003FFF`**: VirtIO-Balloon MMIO.
- **`0x10004000 - 0x10004FFF`**: VirtIO-GPU MMIO.
- **`0x20000000 - 0x2000FFFF`**: NVMe Storage Controller BAR0.
- **`0x80000000 - 0xFFFFFFFF`**: Guest RAM Physical Execution Memory.
- **`0xE0000000 - 0xEFFFFFFF`**: PCI Express ECAM Memory Region.
- **`0xFFF00000 - 0xFFFFFFFF`**: UEFI (OVMF) Firmware ROM space.

### Memory Reclamation & Ballooning
- Un-accessed pages remain unallocated in host RAM (`pages_.find(page_index) == end()`).
- The `ResourceScheduler` periodically triggers `reclaim_idle_pages()`, unmapping host memory commitment for pages untouched for longer than `max_idle_duration`.
- The `VirtIOBalloonDevice` allows guest OS kernels to voluntarily yield memory back to the host system via hypercall.

---

## 2. vCPU Subsystem & Instruction Decoder (`VCPU`)

- **Registers:** 32 general-purpose 64-bit registers (`x0` hardwired to 0, `x1`–`x31` for general execution).
- **CSRs:** Control and Status Registers (`mstatus`, `satp`, `mtvec`, `mepc`, `mcause`, `cycle`, `time`).
- **Paging Translation:** When `satp.MODE == 8`, virtual addresses are translated using 3-level page tables (`Sv39`).
- **Wait For Interrupt (`WFI`):** When the guest vCPU executes `WFI`, the vCPU transitions to `VCPUState::IDLE_WAIT`, signaling the host thread to enter low-power sleep.

---

## 3. Storage Subsystem (`SparseDisk` & `CoWSparseDisk`)

### `.lime` File Format
- **Magic:** `LIME` (4 bytes)
- **Version:** `uint32_t` (1)
- **Virtual Capacity:** 64-bit integer
- **Block Size:** 64 KB (65,536 bytes)
- **Block Allocation Table:** `total_blocks * uint32_t` file offsets.
- **Data Chunks:** Allocated on demand as guest performs sector write operations.

---

## 4. Host Resource Scheduler (`ResourceScheduler`)

- Calculates active vs. idle vCPU cycle ratios over sliding time windows.
- Dynamically scales host thread sleep time (`0ms` under load up to `15ms` during idle periods).
- Adjusts page trimming intervals based on policy selection:
  - **`AGGRESSIVE`**: Short idle page threshold (20 ticks), aggressive sleep.
  - **`BALANCED`**: Standard workload adaptation.
  - **`LAZY`**: Performance-focused minimal page trimming.
