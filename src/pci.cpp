#include "lime/pci.hpp"
#include <cstring>

namespace lime {

PCIDevice::PCIDevice(uint16_t vendor_id, uint16_t device_id, uint8_t class_code, uint8_t subclass) {
    header_.vendor_id = vendor_id;
    header_.device_id = device_id;
    header_.class_code = class_code;
    header_.subclass = subclass;
    header_.command = 0x0007;
    header_.status = 0x0010;
}

uint32_t PCIDevice::read(uint64_t offset, size_t) {
    if (offset + 4 <= sizeof(header_)) {
        uint32_t val = 0;
        std::memcpy(&val, reinterpret_cast<const uint8_t*>(&header_) + offset, sizeof(val));
        return val;
    }
    return 0;
}

void PCIDevice::write(uint64_t offset, uint32_t value, size_t) {
    if (offset + 4 <= sizeof(header_)) {
        std::memcpy(reinterpret_cast<uint8_t*>(&header_) + offset, &value, sizeof(value));
    }
}

void PCIDevice::set_bar(size_t index, uint32_t address) {
    if (index < 6) {
        header_.bar[index] = address;
    }
}

uint32_t PCIDevice::get_bar(size_t index) const {
    if (index < 6) {
        return header_.bar[index];
    }
    return 0;
}

PCIBus::PCIBus(uint64_t ecam_base) : ecam_base_(ecam_base) {}

uint32_t PCIBus::make_key(uint8_t bus, uint8_t slot, uint8_t func) const {
    return (static_cast<uint32_t>(bus) << 16) | (static_cast<uint32_t>(slot) << 8) | static_cast<uint32_t>(func);
}

bool PCIBus::attach_device(uint8_t bus, uint8_t slot, uint8_t func, std::shared_ptr<PCIDevice> dev) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (slot < 32 && func < 8) {
        uint32_t key = make_key(bus, slot, func);
        devices_[key] = dev;
        return true;
    }
    return false;
}

uint32_t PCIBus::read(uint64_t offset, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint8_t bus = static_cast<uint8_t>((offset >> 20) & 0xFF);
    uint8_t slot = static_cast<uint8_t>((offset >> 15) & 0x1F);
    uint8_t func = static_cast<uint8_t>((offset >> 12) & 0x07);
    uint64_t reg_offset = offset & 0xFFF;

    uint32_t key = make_key(bus, slot, func);
    auto it = devices_.find(key);
    if (it != devices_.end() && it->second) {
        return it->second->read(reg_offset, size);
    }
    return 0xFFFFFFFF;
}

void PCIBus::write(uint64_t offset, uint32_t value, size_t size) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint8_t bus = static_cast<uint8_t>((offset >> 20) & 0xFF);
    uint8_t slot = static_cast<uint8_t>((offset >> 15) & 0x1F);
    uint8_t func = static_cast<uint8_t>((offset >> 12) & 0x07);
    uint64_t reg_offset = offset & 0xFFF;

    uint32_t key = make_key(bus, slot, func);
    auto it = devices_.find(key);
    if (it != devices_.end() && it->second) {
        it->second->write(reg_offset, value, size);
    }
}

}
