#ifndef LIME_TPM_HPP
#define LIME_TPM_HPP

#include <cstdint>
#include <string>
#include <memory>
#include <mutex>
#include "lime/devices.hpp"

namespace lime {

class TPMDevice : public Device {
public:
    explicit TPMDevice(uint64_t base_addr = 0x10003000);
    ~TPMDevice() override = default;

    std::string name() const override { return "TPM2.0"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x1000; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

private:
    uint64_t base_addr_;
    mutable std::mutex mutex_;
};

}

#endif
