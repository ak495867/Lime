#include "lime/tpm.hpp"

namespace lime {

TPMDevice::TPMDevice(uint64_t base_addr) : base_addr_(base_addr) {}

uint32_t TPMDevice::read(uint64_t offset, size_t) {


    switch (offset) {
    case 0x00: return 0x4150544D;  
    case 0x04: return 2;           
    case 0x08: return 1;           
    default: return 0;
    }
}

void TPMDevice::write(uint64_t offset, uint32_t value, size_t) {


    (void)offset; (void)value;
}

};  
