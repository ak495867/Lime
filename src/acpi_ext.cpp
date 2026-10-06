#include "lime/acpi_ext.hpp"

namespace lime {

ACPITimerDevice::ACPITimerDevice(uint64_t base_addr) : base_addr_(base_addr) {}

uint32_t ACPITimerDevice::read(uint64_t offset, size_t) {
    switch (offset) {
    case 0x00: return 0x54494D45;  
    case 0x08: return static_cast<uint32_t>(counter_);
    case 0x0C: return static_cast<uint32_t>(counter_ >> 32);
    case 0x10: return divider_;
    default: return 0;
    }
}

void ACPITimerDevice::write(uint64_t offset, uint32_t value, size_t) {
    switch (offset) {
    case 0x08:  
        counter_ = (counter_ & 0xFFFFFFFF00000000ULL) | value;
        break;
    case 0x0C:  
        counter_ = (counter_ & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    case 0x10:  
        divider_ = value;
        break;
    default:
        break;
    }
}

void ACPITimerDevice::tick() {
    if (divider_ > 0) {
        counter_ += 1000000000 / divider_;  
    } else {
        counter_++;
    }
}

ACPIButtonDevice::ACPIButtonDevice(uint64_t base_addr) : base_addr_(base_addr) {}

uint32_t ACPIButtonDevice::read(uint64_t offset, size_t) {
    switch (offset) {
    case 0x00: return 0x42544E01;  
    case 0x04: return status_;
    case 0x08: return enable_;
    default: return 0;
    }
}

void ACPIButtonDevice::write(uint64_t offset, uint32_t value, size_t) {
    switch (offset) {
    case 0x04:  
        status_ = value;
        break;
    case 0x08:  
        enable_ = value;
        break;
    default:
        break;
    }
}

void ACPIButtonDevice::set_power_button_handler(std::function<void()> handler) {
    power_handler_ = handler;
}

void ACPIButtonDevice::set_sleep_button_handler(std::function<void()> handler) {
    sleep_handler_ = handler;
}

void ACPIButtonDevice::set_reset_button_handler(std::function<void()> handler) {
    reset_handler_ = handler;
}

};  
