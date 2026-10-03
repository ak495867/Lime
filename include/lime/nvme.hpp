#ifndef LIME_NVME_HPP
#define LIME_NVME_HPP

#include <cstdint>
#include <vector>
#include <memory>
#include <mutex>
#include "lime/pci.hpp"
#include "lime/storage.hpp"

namespace lime {

#pragma pack(push, 1)
struct NVMeControllerRegs {
    uint64_t cap{0x0000000100000000ULL};
    uint32_t vs{0x00010300};
    uint32_t intms{0};
    uint32_t intmc{0};
    uint32_t cc{0};
    uint32_t reserved1{0};
    uint32_t csts{0};
    uint32_t nssr{0};
    uint32_t aqa{0};
    uint64_t asq{0};
    uint64_t acq{0};
};
#pragma pack(pop)

class NVMeController : public PCIDevice {
public:
    explicit NVMeController(std::shared_ptr<SparseDisk> disk, uint64_t base_addr = 0x20000000);
    ~NVMeController() override = default;

    std::string name() const override { return "NVMe-Controller"; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

    void process_admin_sq();
    void process_io_sq();

    uint64_t capacity_bytes() const;

private:
    std::shared_ptr<SparseDisk> disk_;
    NVMeControllerRegs regs_;
    uint32_t admin_sq_head_{0};
    uint32_t admin_cq_tail_{0};
    mutable std::mutex mutex_;
};

}

#endif
