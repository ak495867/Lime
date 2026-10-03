#include "lime/vm.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <fstream>
#include <cstring>

namespace lime {

VirtualMachine::VirtualMachine(const VMConfig& config) : config_(config) {}

VirtualMachine::~VirtualMachine() {
    stop();
}

bool VirtualMachine::init() {
    size_t ram_bytes = config_.ram_size_mb * 1024 * 1024;
    memory_ = std::make_shared<MemoryManager>(ram_bytes, 4096);

    bus_ = std::make_shared<DeviceBus>();

    if (config_.enable_pci) {
        pci_bus_ = std::make_shared<PCIBus>(0xE0000000);
        bus_->register_device(pci_bus_);
    }

    if (config_.enable_acpi) {
        ACPITableBuilder::generate_tables(memory_, 0x000F0000, config_.cpu_count);
    }

    if (!config_.firmware_path.empty()) {
        FirmwareLoader::load_firmware(memory_, config_.firmware_path, FirmwareType::UEFI_OVMF, 0xFFF00000);
    }

    clint_ = std::make_shared<ClintDevice>(0x02000000);
    bus_->register_device(clint_);

    plic_ = std::make_shared<PlicDevice>(0x0C000000);
    bus_->register_device(plic_);

    console_ = std::make_shared<UartConsoleDevice>(0x10000000);
    bus_->register_device(console_);

    if (!config_.delta_disk_path.empty() && !config_.sparse_disk_path.empty()) {
        cow_disk_ = std::make_shared<CoWSparseDisk>();
        if (!cow_disk_->open(config_.sparse_disk_path, config_.delta_disk_path)) {
            CoWSparseDisk::create_overlay(config_.sparse_disk_path, config_.delta_disk_path);
            cow_disk_->open(config_.sparse_disk_path, config_.delta_disk_path);
        }
    } else if (!config_.sparse_disk_path.empty()) {
        disk_ = std::make_shared<SparseDisk>();
        if (!disk_->open(config_.sparse_disk_path)) {
            SparseDisk::create(config_.sparse_disk_path, 10ULL * 1024 * 1024 * 1024);
            disk_->open(config_.sparse_disk_path);
        }
        block_device_ = std::make_shared<VirtIOBlockDevice>(disk_, 0x10001000);
        bus_->register_device(block_device_);

        if (config_.enable_nvme) {
            nvme_ = std::make_shared<NVMeController>(disk_, memory_, 0x20000000);
            bus_->register_device(nvme_);
            if (pci_bus_) {
                pci_bus_->attach_device(0, 4, 0, nvme_);
            }
        }
    }

    if (config_.enable_net) {
        net_device_ = std::make_shared<VirtIONetDevice>(0x10002000);
        bus_->register_device(net_device_);

        net_bridge_ = std::make_shared<HostNetBridge>(net_device_);
        net_bridge_->start_bridge(8888);
    }

    balloon_device_ = std::make_shared<VirtIOBalloonDevice>(memory_, 0x10003000);
    bus_->register_device(balloon_device_);

    if (config_.enable_graphics) {
        gpu_device_ = std::make_shared<VirtIOGraphicsDevice>(0x10004000);
        bus_->register_device(gpu_device_);
    }

    if (config_.target_arch == TargetArch::X86_64) {
        x86_vcpu_ = std::make_shared<X86CPUDecoder>(memory_, bus_);
        x86_vcpu_->reset(0x7C00);
    } else {
        vcpu_ = std::make_shared<VCPU>(0, memory_, bus_);
        vcpu_->attach_clint(clint_);
        vcpu_->attach_plic(plic_);
        vcpu_->reset(entry_point_);
    }

    hypervisor_ = std::make_shared<HostHypervisor>();
    if (config_.use_hardware_hypervisor) {
        hypervisor_->initialize(HypervisorType::WHPX_WINDOWS);
        if (hypervisor_->is_active() && vcpu_) {
            vcpu_->set_virt_mode(VirtMode::HARDWARE_VIRTUALIZATION);
        }
    }

    if (vcpu_) {
        scheduler_ = std::make_shared<ResourceScheduler>(vcpu_, memory_, config_.policy);
    }

    return true;
}

bool VirtualMachine::load_binary(const std::vector<uint8_t>& code, uint64_t load_addr) {
    if (!memory_) return false;
    entry_point_ = load_addr;
    if (vcpu_) {
        vcpu_->reset(entry_point_);
    } else if (x86_vcpu_) {
        x86_vcpu_->reset(load_addr);
    }
    return memory_->write_bytes(load_addr, code.data(), code.size());
}

bool VirtualMachine::load_lime_image(const std::string& image_path) {
    std::ifstream in(image_path, std::ios::binary);
    if (!in.is_open()) return false;

    LimeSparseDiskHeader hdr;
    in.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (in.gcount() != sizeof(hdr)) return false;

    if (hdr.magic[0] == 'L' && hdr.magic[1] == 'I' && hdr.magic[2] == 'M' && hdr.magic[3] == 'E') {
        config_.sparse_disk_path = image_path;
        if (!disk_) {
            disk_ = std::make_shared<SparseDisk>();
        }
        disk_->open(image_path);
        if (!block_device_) {
            block_device_ = std::make_shared<VirtIOBlockDevice>(disk_, 0x10001000);
            bus_->register_device(block_device_);
        }
    }

    std::vector<uint8_t> code = LimeImageBuilder::generate_mini_os_code();
    return load_binary(code, 0x80000000);
}

void VirtualMachine::run() {
    running_ = true;
    if (scheduler_) {
        scheduler_->start();
    }

    const size_t batch_cycles = 1000;

    while (running_) {
        if (config_.target_arch == TargetArch::X86_64) {
            if (!x86_vcpu_ || !x86_vcpu_->step()) {
                break;
            }
        } else {
            if (vcpu_->state() == VCPUState::HALTED) {
                break;
            }

            bool was_idle = (vcpu_->state() == VCPUState::IDLE_WAIT);
            if (!was_idle) {
                size_t executed = vcpu_->run_cycles(batch_cycles);
                if (executed == 0) {
                    was_idle = true;
                }
            }

            scheduler_->notify_cycle(was_idle);
            bus_->tick_all();

            uint32_t sleep_ms = scheduler_->calculate_sleep_duration_ms();
            if (sleep_ms > 0) {
                std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
            }
        }
    }

    if (scheduler_) {
        scheduler_->stop();
    }
    running_ = false;
}

void VirtualMachine::stop() {
    running_ = false;
    if (vcpu_) {
        vcpu_->set_state(VCPUState::HALTED);
    }
    if (scheduler_) {
        scheduler_->stop();
    }
}

ResourceMetrics VirtualMachine::get_metrics() const {
    if (scheduler_) {
        return scheduler_->metrics();
    }
    return ResourceMetrics{};
}

std::shared_ptr<MemoryManager> VirtualMachine::memory() const { return memory_; }
std::shared_ptr<SparseDisk> VirtualMachine::disk() const { return disk_; }
std::shared_ptr<CoWSparseDisk> VirtualMachine::cow_disk() const { return cow_disk_; }
std::shared_ptr<DeviceBus> VirtualMachine::bus() const { return bus_; }
std::shared_ptr<PCIBus> VirtualMachine::pci_bus() const { return pci_bus_; }
std::shared_ptr<VCPU> VirtualMachine::vcpu() const { return vcpu_; }
std::shared_ptr<X86CPUDecoder> VirtualMachine::x86_vcpu() const { return x86_vcpu_; }
std::shared_ptr<ResourceScheduler> VirtualMachine::scheduler() const { return scheduler_; }
std::shared_ptr<HostHypervisor> VirtualMachine::hypervisor() const { return hypervisor_; }
const VMConfig& VirtualMachine::config() const { return config_; }

std::vector<uint8_t> LimeImageBuilder::generate_mini_os_code() {
    std::vector<uint32_t> insts = {
        0x100002B7,
        0x04828293,
        0x00528023,
        0x04528293,
        0x00528023,
        0x04C28293,
        0x00528023,
        0x04C28293,
        0x00528023,
        0x04F28293,
        0x00528023,
        0x02C28293,
        0x00528023,
        0x02028293,
        0x00528023,
        0x05728293,
        0x00528023,
        0x06F28293,
        0x00528023,
        0x07228293,
        0x00528023,
        0x06C28293,
        0x00528023,
        0x06428293,
        0x00528023,
        0x02128293,
        0x00528023,
        0x00A28293,
        0x00528023,
        0x10500073,
        0x00000073
    };

    std::vector<uint8_t> code(insts.size() * sizeof(uint32_t));
    std::memcpy(code.data(), insts.data(), code.size());
    return code;
}

bool LimeImageBuilder::create_default_mini_os_image(const std::string& output_path, uint64_t disk_size_mb) {
    uint64_t bytes = disk_size_mb * 1024 * 1024;
    if (!SparseDisk::create(output_path, bytes)) {
        return false;
    }

    SparseDisk disk;
    if (!disk.open(output_path)) {
        return false;
    }

    std::vector<uint8_t> os_code = generate_mini_os_code();
    std::vector<uint8_t> sector_data(SparseDisk::SECTOR_SIZE, 0);
    std::memcpy(sector_data.data(), os_code.data(), std::min(os_code.size(), static_cast<size_t>(SparseDisk::SECTOR_SIZE)));

    disk.write_sectors(0, 1, sector_data.data());
    disk.close();
    return true;
}

}
