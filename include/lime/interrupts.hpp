#ifndef LIME_INTERRUPTS_HPP
#define LIME_INTERRUPTS_HPP

#include <cstdint>
#include <vector>
#include <mutex>
#include "lime/devices.hpp"

namespace lime {

class ClintDevice : public Device {
public:
    explicit ClintDevice(uint64_t base_addr = 0x02000000);
    ~ClintDevice() override = default;

    std::string name() const override { return "CLINT"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x10000; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;
    void tick() override;

    bool timer_interrupt_pending() const;
    bool software_interrupt_pending() const;

private:
    uint64_t base_addr_;
    uint32_t msip_{0};
    uint64_t mtimecmp_{0xFFFFFFFFFFFFFFFFULL};
    uint64_t mtime_{0};
    mutable std::mutex mutex_;
};

class PlicDevice : public Device {
public:
    explicit PlicDevice(uint64_t base_addr = 0x0C000000);
    ~PlicDevice() override = default;

    std::string name() const override { return "PLIC"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x4000000; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

    void set_irq(uint32_t irq_num, bool level);
    uint32_t claim_irq();
    void complete_irq(uint32_t irq_num);

    bool external_interrupt_pending() const;

private:
    uint64_t base_addr_;
    std::vector<uint32_t> priority_;
    uint32_t pending_{0};
    uint32_t enable_{0};
    uint32_t threshold_{0};
    uint32_t claimed_irq_{0};
    mutable std::mutex mutex_;
};

}

#endif
