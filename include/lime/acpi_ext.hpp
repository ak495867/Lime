#ifndef LIME_ACPI_EXT_HPP
#define LIME_ACPI_EXT_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include "lime/devices.hpp"
#include "lime/memory.hpp"

namespace lime {

class ACPITimerDevice : public Device {
public:
    explicit ACPITimerDevice(uint64_t base_addr = 0xFED00000);
    ~ACPITimerDevice() override = default;

    std::string name() const override { return "ACPI-Timer"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x400; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

    void tick();  

    uint64_t get_counter() const { return counter_; }

private:
    uint64_t base_addr_;
    uint64_t counter_{0};
    uint32_t divider_{0};
    mutable std::mutex mutex_;
};

class ACPIButtonDevice : public Device {
public:
    explicit ACPIButtonDevice(uint64_t base_addr = 0xFED80000);
    ~ACPIButtonDevice() override = default;

    std::string name() const override { return "ACPI-Button"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x100; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

    void set_power_button_handler(std::function<void()> handler);
    void set_sleep_button_handler(std::function<void()> handler);
    void set_reset_button_handler(std::function<void()> handler);

private:
    uint64_t base_addr_;
    uint32_t status_{0};
    uint32_t enable_{0};
    std::function<void()> power_handler_;
    std::function<void()> sleep_handler_;
    std::function<void()> reset_handler_;
    mutable std::mutex mutex_;
};

}

#endif
