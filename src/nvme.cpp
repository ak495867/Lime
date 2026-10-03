#include "lime/nvme.hpp"
#include <cstring>
#include <iostream>

namespace lime {

NVMeController::NVMeController(std::shared_ptr<SparseDisk> disk, std::shared_ptr<MemoryManager> mem, uint64_t base_addr)
    : PCIDevice(0x1B4B, 0x0108, 0x01, 0x08), disk_(disk), mem_(mem) {
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
    case 0x24: return regs_.aqa;
    case 0x28: return static_cast<uint32_t>(regs_.asq & 0xFFFFFFFF);
    case 0x2C: return static_cast<uint32_t>(regs_.asq >> 32);
    case 0x30: return static_cast<uint32_t>(regs_.acq & 0xFFFFFFFF);
    case 0x34: return static_cast<uint32_t>(regs_.acq >> 32);
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
    case 0x24: regs_.aqa = value; break;
    case 0x28: regs_.asq = (regs_.asq & 0xFFFFFFFF00000000ULL) | value; break;
    case 0x2C: regs_.asq = (regs_.asq & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(value) << 32); break;
    case 0x30: regs_.acq = (regs_.acq & 0xFFFFFFFF00000000ULL) | value; break;
    case 0x34: regs_.acq = (regs_.acq & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(value) << 32); break;
    case 0x1000:
        admin_sq_head_ = value & 0xFFFF;
        process_admin_sq();
        break;
    default:
        break;
    }
}

void NVMeController::process_admin_sq() {
    if (!mem_ || regs_.asq == 0 || regs_.acq == 0) return;
    
    uint16_t sq_size = (regs_.aqa & 0xFFF) + 1;
    uint32_t sq_entry_size = 64;
    uint32_t cq_entry_size = 16;
    
    std::vector<uint8_t> sqe(sq_entry_size);
    mem_->read_bytes(regs_.asq + admin_sq_head_ * sq_entry_size, sqe.data(), sq_entry_size);
    
    uint8_t opcode = sqe[0];
    uint16_t cid = sqe[2] | (sqe[3] << 8);
    
    std::vector<uint8_t> cqe(cq_entry_size, 0);
    cqe[12] = cid & 0xFF;
    cqe[13] = (cid >> 8) & 0xFF;
    cqe[14] = 0;
    cqe[15] = 1; 

    mem_->write_bytes(regs_.acq + admin_cq_tail_ * cq_entry_size, cqe.data(), cq_entry_size);
    
    admin_cq_tail_ = (admin_cq_tail_ + 1) % sq_size;
}

void NVMeController::process_io_sq() {}

uint64_t NVMeController::capacity_bytes() const {
    return disk_ ? disk_->capacity_bytes() : 0;
}

}
