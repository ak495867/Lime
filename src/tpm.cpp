#include "lime/tpm.hpp"

namespace lime {

TPMDevice::TPMDevice(uint64_t base_addr) : base_addr_(base_addr) {}

uint32_t TPMDevice::read(uint64_t offset, size_t) {
    // Simplified TPM 2.0 register read
    // In a full implementation, would read TPM registers and command responses
    switch (offset) {
    case 0x00: return 0x4150544D;  // "TPM " magic
    case 0x04: return 2;           // TPM major version
    case 0x08: return 1;           // TPM minor version
    default: return 0;
    }
}

void TPMDevice::write(uint64_t offset, uint32_t value, size_t) {
    // Simplified TPM 2.0 register write
    // In a full implementation, would handle TPM command submissions
    (void)offset; (void)value;
}

};  // namespace lime