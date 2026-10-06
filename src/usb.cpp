#include "lime/usb.hpp"

namespace lime {

VirtIOUSBDevice::VirtIOUSBDevice(uint64_t base_addr) 
    : base_addr_(base_addr), attached_devices_(4, "") {}

uint32_t VirtIOUSBDevice::read(uint64_t offset, size_t) {
    switch (offset) {
    case 0x00: return 0x74726976;  // 'VIRT' magic
    case 0x04: return 2;           // 2 bytes
    case 0x08: return 1;           // 1 device descriptor
    case 0x70: return status_;      // Status register
    default: return 0;
    }
}

void VirtIOUSBDevice::write(uint64_t offset, uint32_t value, size_t) {
    switch (offset) {
    case 0x70:
        status_ = value;
        break;
    default:
        break;
    }
}

bool VirtIOUSBDevice::attach_device(uint8_t port, const std::string& device_type) {
    if (port >= 4 || !(port_mask_ & (1 << port))) return false;
    attached_devices_[port] = device_type;
    port_mask_ |= (1 << port);
    return true;
}

bool VirtIOUSBDevice::detach_device(uint8_t port) {
    if (port >= 4 || !(port_mask_ & (1 << port))) return false;
    attached_devices_[port].clear();
    port_mask_ &= ~(1 << port);
    return true;
}

bool VirtIOUSBDevice::is_port_attached(uint8_t port) const {
    return (port < 4) && (port_mask_ & (1 << port)) && !attached_devices_[port].empty();
}

};  // namespace lime