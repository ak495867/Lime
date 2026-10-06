#ifndef LIME_VM_HPP
#define LIME_VM_HPP

#include <cstdint>
#include <string>
#include <memory>
#include <atomic>
#include <vector>
#include "lime/memory.hpp"
#include "lime/storage.hpp"
#include "lime/cow_disk.hpp"
#include "lime/devices.hpp"
#include "lime/interrupts.hpp"
#include "lime/hypervisor.hpp"
#include "lime/net_bridge.hpp"
#include "lime/acpi.hpp"
#include "lime/pci.hpp"
#include "lime/firmware.hpp"
#include "lime/nvme.hpp"
#include "lime/x86_cpu.hpp"
#include "lime/vcpu.hpp"
#include "lime/scheduler.hpp"

namespace lime {

struct VMConfig {
    size_t ram_size_mb{256};
    uint32_t cpu_count{1};
    TargetArch target_arch{TargetArch::RISCV64};
    std::string sparse_disk_path;
    std::string delta_disk_path;
    std::string firmware_path;
    std::string ovmf_vars_path;
    std::string kernel_path;
    std::string initrd_path;
    std::string cmdline;
    AllocationPolicy policy{AllocationPolicy::BALANCED};
    bool enable_net{true};
    bool enable_graphics{true};
    bool enable_pci{true};
    bool enable_nvme{true};
    bool enable_acpi{true};
    bool headless{false};
    bool use_hardware_hypervisor{false};
};

class VirtualMachine {
public:
    explicit VirtualMachine(const VMConfig& config);
    ~VirtualMachine();

    bool init();
    bool load_binary(const std::vector<uint8_t>& code, uint64_t load_addr = 0x80000000);
    bool load_lime_image(const std::string& image_path);

    void run();
    void stop();

    ResourceMetrics get_metrics() const;

    std::shared_ptr<MemoryManager> memory() const;
    std::shared_ptr<SparseDisk> disk() const;
    std::shared_ptr<CoWSparseDisk> cow_disk() const;
    std::shared_ptr<DeviceBus> bus() const;
    std::shared_ptr<PCIBus> pci_bus() const;
    std::shared_ptr<VCPU> vcpu() const;
    std::vector<std::shared_ptr<VCPU>> vcpus() const;
    std::shared_ptr<X86CPUDecoder> x86_vcpu() const;
    std::shared_ptr<ResourceScheduler> scheduler() const;
    std::shared_ptr<HostHypervisor> hypervisor() const;
    const VMConfig& config() const;

private:
    VMConfig config_;
    std::shared_ptr<MemoryManager> memory_;
    std::shared_ptr<SparseDisk> disk_;
    std::shared_ptr<CoWSparseDisk> cow_disk_;
    std::shared_ptr<DeviceBus> bus_;
    std::shared_ptr<PCIBus> pci_bus_;
    std::vector<std::shared_ptr<VCPU>> vcpus_;
    std::shared_ptr<VCPU> vcpu_;
    std::shared_ptr<X86CPUDecoder> x86_vcpu_;
    std::shared_ptr<ResourceScheduler> scheduler_;
    std::shared_ptr<HostHypervisor> hypervisor_;
    std::shared_ptr<ClintDevice> clint_;
    std::shared_ptr<PlicDevice> plic_;
    std::shared_ptr<NVMeController> nvme_;
    std::shared_ptr<HostNetBridge> net_bridge_;
    std::shared_ptr<UartConsoleDevice> console_;
    std::shared_ptr<VirtIOBlockDevice> block_device_;
    std::shared_ptr<VirtIONetDevice> net_device_;
    std::shared_ptr<VirtIOBalloonDevice> balloon_device_;
    std::shared_ptr<VirtIOGraphicsDevice> gpu_device_;

    std::atomic<bool> running_{false};
    uint64_t entry_point_{0x80000000};
    std::vector<std::thread> vcpu_threads_;
};

class LimeImageBuilder {
public:
    static bool create_default_mini_os_image(const std::string& output_path, uint64_t disk_size_mb = 100);
    static std::vector<uint8_t> generate_mini_os_code();
};

}

#endif
