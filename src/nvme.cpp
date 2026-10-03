#include "lime/nvme.hpp"
#include <cstring>

namespace lime {

NVMeController::NVMeController(std::shared_ptr<SparseDisk> disk, uint64_t base_addr)
    : PCIDevice(0x1B4B, 0x0108, 0x01, 0x08), disk_(disk) {
    base_addr_ = base_addr;
}

uint32_t NVMeController::read(uint64_t offset, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset < sizeof(PCIDeviceHeader)) {
        return PCIDevice::read(offset, size);
    }
    uint64_t bar_offset = offset - sizeof(PCIDeviceHeader);
    switch (bar_offset) {
    case 0x00: return static_cast<uint32_t>(regs_.cap & 0xFFFFFFFF);
    case 0x04: return static_cast<uint32_t>(regs_.cap >> 32);
    case 0x08: return regs_.vs;
    case 0x14: return regs_.cc;
    case 0x1C: return regs_.csts;
    default: return 0;
    }
}

void NVMeController::write(uint64_t offset, uint32_t value, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset < sizeof(PCIDeviceHeader)) {
        PCIDevice::write(offset, value, size);
        return;
    }
    uint64_t bar_offset = offset - sizeof(PCIDeviceHeader);
    switch (bar_offset) {
    case 0x14:
        regs_.cc = value;
        if (value & 1) {
            regs_.csts |= 1;
        } else {
            regs_.csts &= ~1ULL;
        }
        break;
    case 0x24:
        regs_.aqa = value;
        break;
    case 0x28:
        regs_.asq = (regs_.asq & 0xFFFFFFFF00000000ULL) | value;
        break;
    case 0x2C:
        regs_.asq = (regs_.asq & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    case 0x1000:
        admin_sq_head_ = value & 0xFFFF;
        process_admin_sq();
        break;
    default:
        break;
    }
}

void NVMeController::process_admin_sq() {}
void NVMeController::process_io_sq() {}

uint64_t NVMeController::capacity_bytes() const {
    return disk_ ? disk_->capacity_bytes() : 0;
}

}
