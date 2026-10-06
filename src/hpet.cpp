#include "lime/hpet.hpp"

namespace lime {

HPETDevice::HPETDevice(uint64_t base_addr) : base_addr_(base_addr) {}

uint32_t HPETDevice::read(uint64_t offset, size_t) {
    switch (offset) {
    case 0x00:  
        return 0x48504554;  
    case 0x10:  
        return config_;
    case 0xF0:  
        return static_cast<uint32_t>(main_counter_);
    case 0xF8:  
        return static_cast<uint32_t>(main_counter_ >> 32);
    default:
        return 0;
    }
}

void HPETDevice::write(uint64_t offset, uint32_t value, size_t) {
    switch (offset) {
    case 0x10:  
        config_ = value;
        break;
    case 0xF0:  
        main_counter_ = (main_counter_ & 0xFFFFFFFF00000000ULL) | value;
        break;
    case 0xF8:  
        main_counter_ = (main_counter_ & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    default:
        break;
    }
}

void HPETDevice::tick() {
    main_counter_++;
}
}
