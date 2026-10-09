#ifndef LIME_HPET_HPP
#define LIME_HPET_HPP

#include <cstdint>
#include <string>
#include <memory>
#include <mutex>
#include "lime/devices.hpp"

namespace lime {

class HPETDevice : public Device {
public:
    explicit HPETDevice(uint64_t base_addr = 0xFED00000);
    ~HPETDevice() override = default;

    std::string name() const override { return "HPET"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x400; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

    void tick();  

private:
    uint64_t base_addr_;
    uint64_t main_counter_{0};
    uint64_t comparator_[8]{0};
    uint32_t config_{0};
    uint32_t int_status_{0};
    mutable std::mutex mutex_;
};

}

#endif
