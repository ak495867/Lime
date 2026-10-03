#ifndef LIME_PCI_HPP
#define LIME_PCI_HPP

#include <cstdint>
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>
#include "lime/devices.hpp"

namespace lime {

#pragma pack(push, 1)
struct PCIDeviceHeader {
    uint16_t vendor_id{0xFFFF};
    uint16_t device_id{0xFFFF};
    uint16_t command{0};
    uint16_t status{0};
    uint8_t revision_id{0};
    uint8_t prog_if{0};
    uint8_t subclass{0};
    uint8_t class_code{0};
    uint8_t cache_line_size{0};
    uint8_t latency_timer{0};
    uint8_t header_type{0};
    uint8_t bist{0};
    uint32_t bar[6]{0, 0, 0, 0, 0, 0};
    uint32_t cardbus_cis_pointer{0};
    uint16_t subsys_vendor_id{0};
    uint16_t subsys_id{0};
    uint32_t expansion_rom_base{0};
    uint8_t capabilities_pointer{0};
    uint8_t reserved[7]{0};
    uint8_t interrupt_line{0};
    uint8_t interrupt_pin{0};
    uint8_t min_gnt{0};
    uint8_t max_lat{0};
};
#pragma pack(pop)

class PCIDevice : public Device {
public:
    PCIDevice(uint16_t vendor_id, uint16_t device_id, uint8_t class_code, uint8_t subclass);
    ~PCIDevice() override = default;

    std::string name() const override { return "PCIDevice"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 256; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

    void set_bar(size_t index, uint32_t address);
    uint32_t get_bar(size_t index) const;

protected:
    PCIDeviceHeader header_;
    uint64_t base_addr_{0};
};

class PCIBus : public Device {
public:
    explicit PCIBus(uint64_t ecam_base = 0xE0000000);
    ~PCIBus() override = default;

    std::string name() const override { return "PCIBus-ECAM"; }
    uint64_t base_address() const override { return ecam_base_; }
    uint64_t size() const override { return 256 * 1024 * 1024; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

    bool attach_device(uint8_t bus, uint8_t slot, uint8_t func, std::shared_ptr<PCIDevice> dev);

private:
    uint32_t make_key(uint8_t bus, uint8_t slot, uint8_t func) const;

    uint64_t ecam_base_;
    std::unordered_map<uint32_t, std::shared_ptr<PCIDevice>> devices_;
    mutable std::mutex mutex_;
};

}

#endif
