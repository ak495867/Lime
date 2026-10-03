#include "lime/interrupts.hpp"

namespace lime {

ClintDevice::ClintDevice(uint64_t base_addr) : base_addr_(base_addr) {}

uint32_t ClintDevice::read(uint64_t offset, size_t) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset == 0x0000) return msip_;
    if (offset == 0x4000) return static_cast<uint32_t>(mtimecmp_ & 0xFFFFFFFF);
    if (offset == 0x4004) return static_cast<uint32_t>(mtimecmp_ >> 32);
    if (offset == 0xBFFF8) return static_cast<uint32_t>(mtime_ & 0xFFFFFFFF);
    if (offset == 0xBFFFC) return static_cast<uint32_t>(mtime_ >> 32);
    return 0;
}

void ClintDevice::write(uint64_t offset, uint32_t value, size_t) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset == 0x0000) {
        msip_ = value & 1;
    } else if (offset == 0x4000) {
        mtimecmp_ = (mtimecmp_ & 0xFFFFFFFF00000000ULL) | value;
    } else if (offset == 0x4004) {
        mtimecmp_ = (mtimecmp_ & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
    } else if (offset == 0xBFFF8) {
        mtime_ = (mtime_ & 0xFFFFFFFF00000000ULL) | value;
    } else if (offset == 0xBFFFC) {
        mtime_ = (mtime_ & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
    }
}

void ClintDevice::tick() {
    std::lock_guard<std::mutex> lock(mutex_);
    mtime_++;
}

bool ClintDevice::timer_interrupt_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return mtime_ >= mtimecmp_;
}

bool ClintDevice::software_interrupt_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return msip_ != 0;
}

PlicDevice::PlicDevice(uint64_t base_addr) : base_addr_(base_addr) {
    priority_.resize(32, 0);
}

uint32_t PlicDevice::read(uint64_t offset, size_t) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset >= 0x04 && offset < 0x80) {
        size_t idx = offset / 4;
        if (idx < priority_.size()) return priority_[idx];
    }
    if (offset == 0x1000) return pending_;
    if (offset == 0x2000) return enable_;
    if (offset == 0x200000) return threshold_;
    if (offset == 0x200004) return claimed_irq_;
    return 0;
}

void PlicDevice::write(uint64_t offset, uint32_t value, size_t) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (offset >= 0x04 && offset < 0x80) {
        size_t idx = offset / 4;
        if (idx < priority_.size()) priority_[idx] = value;
    } else if (offset == 0x2000) {
        enable_ = value;
    } else if (offset == 0x200000) {
        threshold_ = value;
    } else if (offset == 0x200004) {
        complete_irq(value);
    }
}

void PlicDevice::set_irq(uint32_t irq_num, bool level) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (irq_num > 0 && irq_num < 32) {
        if (level) {
            pending_ |= (1U << irq_num);
        } else {
            pending_ &= ~(1U << irq_num);
        }
    }
}

uint32_t PlicDevice::claim_irq() {
    std::lock_guard<std::mutex> lock(mutex_);
    uint32_t active = pending_ & enable_;
    for (uint32_t i = 1; i < 32; ++i) {
        if (active & (1U << i)) {
            if (priority_[i] > threshold_) {
                pending_ &= ~(1U << i);
                claimed_irq_ = i;
                return i;
            }
        }
    }
    return 0;
}

void PlicDevice::complete_irq(uint32_t irq_num) {
    if (claimed_irq_ == irq_num) {
        claimed_irq_ = 0;
    }
}

bool PlicDevice::external_interrupt_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return (pending_ & enable_) != 0;
}

}
