#ifndef LIME_NVME_HPP
#define LIME_NVME_HPP

#include <cstdint>
#include <memory>
#include <mutex>
#include "lime/pci.hpp"
#include "lime/storage.hpp"

namespace lime {

class AsyncIOEngine;

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
    explicit NVMeController(std::shared_ptr<SparseDisk> disk, std::shared_ptr<MemoryManager> mem, uint64_t base_addr = 0x20000000);
    ~NVMeController() override = default;

    std::string name() const override { return "NVMe-Controller"; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

    void process_admin_sq();
    void process_io_sq();

    uint64_t capacity_bytes() const;

    // Asynchronous I/O: when an engine is attached, I/O submission queue
    // doorbells return control to the vCPU immediately; completions are
    // harvested from poll_async()/tick().
    void attach_async_engine(std::shared_ptr<AsyncIOEngine> engine);
    size_t poll_async(size_t max_completions = 64);
    void tick() override;

    std::shared_ptr<AsyncIOEngine> async_engine() const { return async_engine_; }

private:
    std::shared_ptr<SparseDisk> disk_;
    std::shared_ptr<MemoryManager> mem_;
    NVMeControllerRegs regs_;
    uint32_t admin_sq_head_{0};
    uint32_t admin_cq_tail_{0};
    mutable std::mutex mutex_;
    std::shared_ptr<AsyncIOEngine> async_engine_;
};

}

#endif
