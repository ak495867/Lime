#ifndef LIME_USB_HPP
#define LIME_USB_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include "lime/devices.hpp"
#include "lime/memory.hpp"

namespace lime {

class VirtIOUSBDevice : public Device {
public:
    explicit VirtIOUSBDevice(uint64_t base_addr = 0x10005000);
    ~VirtIOUSBDevice() override = default;

    std::string name() const override { return "VirtIO-USB"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x1000; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

    bool attach_device(uint8_t port, const std::string& device_type);
    bool detach_device(uint8_t port);
    bool is_port_attached(uint8_t port) const;

private:
    uint64_t base_addr_;
    uint32_t status_{0};
    uint32_t port_mask_{0x0000000F};
    std::vector<std::string> attached_devices_;
    mutable std::mutex mutex_;
};

}

#endif
