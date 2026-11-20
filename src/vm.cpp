#include "lime/vm.hpp"
#include "lime/gdb_stub.hpp"
#include "lime/fs.hpp"
#include "lime/jit_x86.hpp"
#include <iostream>
#include <thread>
#include <chrono>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <limits>
#include <thread>

namespace lime {

VirtualMachine::VirtualMachine(const VMConfig& config) : config_(config) {}

VirtualMachine::~VirtualMachine() {
    stop();
}

bool VirtualMachine::init() {
    if (config_.ram_size_mb == 0 || config_.cpu_count == 0 ||
        config_.ram_size_mb > std::numeric_limits<size_t>::max() / (1024 * 1024) ||
        config_.cpu_count > 64 ||
        (config_.target_arch != TargetArch::RISCV64 && config_.target_arch != TargetArch::X86_64) ||
        (config_.target_arch == TargetArch::X86_64 && config_.cpu_count != 1)) {
        std::cerr << "[LIME] Invalid VM configuration (RAM, CPU count, or architecture)." << std::endl;
        return false;
    }
    if (config_.use_hardware_hypervisor) {
        std::cerr << "[LIME] Hardware virtualization is not implemented by this runtime." << std::endl;
        return false;
    }
    size_t ram_bytes = config_.ram_size_mb * 1024 * 1024;
    memory_ = std::make_shared<MemoryManager>(ram_bytes, 4096);

    bus_ = std::make_shared<DeviceBus>();

    if (config_.enable_pci) {
        pci_bus_ = std::make_shared<PCIBus>(0xE0000000);
        bus_->register_device(pci_bus_);
    }

    if (config_.enable_acpi) {
        if (!ACPITableBuilder::generate_tables(memory_, 0x000F0000, config_.cpu_count)) return false;
    }

    if (!config_.kernel_path.empty()) {
        if (!FirmwareLoader::load_linux_kernel(memory_, config_.kernel_path, config_.initrd_path, config_.cmdline, 0x80200000, &entry_point_)) return false;
    } else if (!config_.firmware_path.empty() && !config_.ovmf_vars_path.empty()) {
        if (!FirmwareLoader::load_ovmf_split(memory_, config_.firmware_path, config_.ovmf_vars_path, 0xFFC00000, 0xFFB00000)) return false;
    } else if (!config_.firmware_path.empty()) {
        if (!FirmwareLoader::load_firmware(memory_, config_.firmware_path, FirmwareType::UEFI_OVMF, 0xFFF00000)) return false;
    }

    clint_ = std::make_shared<ClintDevice>(0x02000000);
    bus_->register_device(clint_);

    plic_ = std::make_shared<PlicDevice>(0x0C000000);
    bus_->register_device(plic_);

    console_ = std::make_shared<UartConsoleDevice>(0x10000000);
    bus_->register_device(console_);

    if (!config_.sparse_disk_path.empty() &&
        !initialize_storage(config_.sparse_disk_path, config_.delta_disk_path)) return false;

    if (!config_.fs_host_path.empty()) {
        auto fs_dev = std::make_shared<VirtIOFSDevice>(memory_, 0x10006000, config_.fs_host_path, config_.fs_tag);
        bus_->register_device(fs_dev);
    }

    if (config_.enable_net) {
        net_device_ = std::make_shared<VirtIONetDevice>(memory_, 0x10002000);
        bus_->register_device(net_device_);
        net_device_->start_worker();

        if (config_.enable_user_net) {
            auto user_net = std::make_shared<UserNetStack>(net_device_);
            for (const auto& [hp, gp] : config_.hostfwd) {
                user_net->add_port_forward(hp, gp, "tcp");
            }
        } else {
            net_bridge_ = std::make_shared<HostNetBridge>(net_device_);
            if (!net_bridge_->start_bridge(8888)) return false;
        }
    }

    balloon_device_ = std::make_shared<VirtIOBalloonDevice>(memory_, 0x10003000);
    bus_->register_device(balloon_device_);

    if (config_.enable_graphics && !config_.headless) {
        gpu_device_ = std::make_shared<VirtIOGraphicsDevice>(0x10004000);
        bus_->register_device(gpu_device_);
        input_device_ = std::make_shared<VirtIOInputDevice>(0x10005000);
        bus_->register_device(input_device_);
    }

    if (config_.target_arch == TargetArch::X86_64) {
        x86_vcpu_ = std::make_shared<X86CPUDecoder>(memory_, bus_);
        x86_vcpu_->reset(entry_point_);
    } else {

        for (uint32_t i = 0; i < config_.cpu_count; ++i) {
            auto vcpu = std::make_shared<VCPU>(i, memory_, bus_);
            vcpu->attach_clint(clint_);
            vcpu->attach_plic(plic_);
            vcpu->reset(entry_point_);
            vcpus_.push_back(vcpu);
            if (i == 0) vcpu_ = vcpu;  
        }
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

    if (config_.gdb_port > 0 && vcpu_) {
        gdb_stub_ = std::make_shared<GDBStub>(vcpu_, memory_, config_.gdb_port);
        gdb_stub_->start();
    }

    return true;
}

bool VirtualMachine::initialize_storage(const std::string& base_path, const std::string& delta_path) {
    if (!delta_path.empty()) {
        if (!std::filesystem::exists(base_path)) {
            std::cerr << "[LIME] CoW base image does not exist: " << base_path << std::endl;
            return false;
        }
        cow_disk_ = std::make_shared<CoWSparseDisk>();
        if (!cow_disk_->open(base_path, delta_path)) {
            if (std::filesystem::exists(delta_path) ||
                !CoWSparseDisk::create_overlay(base_path, delta_path) ||
                !cow_disk_->open(base_path, delta_path)) {
                std::cerr << "[LIME] Failed to open CoW base/delta images." << std::endl;
                return false;
            }
        }
        active_disk_ = cow_disk_;
    } else {
        bool is_sparse = false;
        bool is_qcow2 = false;
        if (std::filesystem::exists(base_path)) {
            std::ifstream probe(base_path, std::ios::binary);
            uint8_t magic[4]{};
            if (probe.read(reinterpret_cast<char*>(magic), 4)) {
                if (std::memcmp(magic, "LIME", 4) == 0) {
                    is_sparse = true;
                } else if (magic[0] == 'Q' && magic[1] == 'F' && magic[2] == 'I' && magic[3] == 0xFB) {
                    is_qcow2 = true;
                }
            }
        } else {
            is_sparse = true;
        }

        if (is_qcow2) {
            auto qcow = std::make_shared<QCOW2Disk>();
            if (!qcow->open(base_path)) {
                std::cerr << "[LIME] Failed to open QCOW2 disk image: " << base_path << std::endl;
                return false;
            }
            active_disk_ = qcow;
        } else if (is_sparse) {
            disk_ = std::make_shared<SparseDisk>();
            if (!disk_->open(base_path)) {
                if (std::filesystem::exists(base_path) ||
                    !SparseDisk::create(base_path, 10ULL * 1024 * 1024 * 1024) ||
                    !disk_->open(base_path)) {
                    std::cerr << "[LIME] Failed to open or create disk image: " << base_path << std::endl;
                    return false;
                }
            }
            active_disk_ = disk_;
        } else {
            auto raw = std::make_shared<RawDisk>();
            if (!raw->open(base_path)) {
                std::cerr << "[LIME] Failed to open raw disk image: " << base_path << std::endl;
                return false;
            }
            active_disk_ = raw;
        }

        async_io_ = AsyncIOEngine::create_best();
        if (async_io_ && !async_io_->open(base_path)) async_io_.reset();
        if (async_io_) {
            std::cout << "[LIME] Async I/O backend active: " << async_io_->backend_name() << std::endl;
        }
    }

    block_device_ = std::make_shared<VirtIOBlockDevice>(active_disk_, memory_, 0x10001000);
    if (async_io_) block_device_->attach_async_engine(async_io_);
    bus_->register_device(block_device_);
    block_device_->start_worker();

    if (config_.enable_nvme) {
        nvme_ = std::make_shared<NVMeController>(active_disk_, memory_, 0x20000000);
        if (async_io_) nvme_->attach_async_engine(async_io_);
        bus_->register_device(nvme_);
        if (pci_bus_ && !pci_bus_->attach_device(0, 4, 0, nvme_)) return false;
    }
    return true;
}

bool VirtualMachine::load_binary(const std::vector<uint8_t>& code, uint64_t load_addr) {
    if (!memory_) return false;
    entry_point_ = load_addr;
    for (auto& vcpu : vcpus_) {
        vcpu->reset(entry_point_);
    }
    if (x86_vcpu_) {
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

    if (hdr.magic[0] != 'L' || hdr.magic[1] != 'I' || hdr.magic[2] != 'M' || hdr.magic[3] != 'E') return false;
    if (active_disk_ && !config_.sparse_disk_path.empty() && config_.sparse_disk_path != image_path) return false;
    if (!active_disk_) {
        config_.sparse_disk_path = image_path;
        if (!initialize_storage(image_path, config_.delta_disk_path)) return false;
    }

    std::vector<uint8_t> code = LimeImageBuilder::generate_mini_os_code();
    return load_binary(code, 0x80000000);
}

void VirtualMachine::run() {
    running_ = true;
    if (scheduler_) {
        scheduler_->start();
    }
    
    if (gpu_device_) {
        gpu_device_->start_display(input_device_);
    }

    if (gdb_stub_ && gdb_stub_->is_connected() && vcpu_) {
        while (running_) {
            if (!gdb_stub_->handle_client()) break;
            if (gdb_stub_->is_single_step()) {
                vcpu_->step();
                bus_->tick_all();
                gdb_stub_->notify_halt(5);
            } else {
                while (running_) {
                    if (gdb_stub_->has_breakpoint(vcpu_->get_pc())) {
                        gdb_stub_->notify_halt(5);
                        break;
                    }
                    if (!vcpu_->step()) {
                        gdb_stub_->notify_halt(5);
                        break;
                    }
                    bus_->tick_all();
                }
            }
        }
    } else if (config_.target_arch == TargetArch::X86_64) {
        while (running_) {
            if (!x86_vcpu_ || !x86_vcpu_->step()) {
                break;
            }
        }
    } else {
        const size_t batch_cycles = 1000;
        std::vector<std::thread> threads;
        
        for (auto& vcpu : vcpus_) {
            threads.emplace_back([this, vcpu, batch_cycles]() {
                while (running_) {
                    if (vcpu->state() == VCPUState::HALTED) {
                        break;
                    }
                    bool was_idle = (vcpu->state() == VCPUState::IDLE_WAIT);
                    if (!was_idle) {
                        size_t executed = vcpu->run_cycles(batch_cycles);
                        if (executed == 0) {
                            was_idle = true;
                        }
                    }
                    if (scheduler_) {
                        scheduler_->notify_cycle(was_idle);
                    }
                    bus_->tick_all();
                    uint32_t sleep_ms = scheduler_->calculate_sleep_duration_ms();
                    if (sleep_ms > 0) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms));
                    }
                }
            });
        }
        
        for (auto& t : threads) {
            if (t.joinable()) {
                t.join();
            }
        }
    }

    if (scheduler_) {
        scheduler_->stop();
    }
    
    if (gpu_device_) {
        gpu_device_->stop_display();
    }
    
    running_ = false;
}

void VirtualMachine::stop() {
    running_ = false;
    for (auto& vcpu : vcpus_) {
        vcpu->set_state(VCPUState::HALTED);
    }
    if (vcpu_) {
        vcpu_->set_state(VCPUState::HALTED);
    }
    if (scheduler_) {
        scheduler_->stop();
    }
    if (block_device_) block_device_->stop_worker();
    if (net_device_) net_device_->stop_worker();
    if (net_bridge_) net_bridge_->stop_bridge();
    if (gpu_device_) gpu_device_->stop_display();
    if (async_io_) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (async_io_->pending() != 0 && std::chrono::steady_clock::now() < deadline) {
            if (async_io_->poll(128) == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (async_io_->pending() != 0) {
            std::cerr << "[LIME] Async I/O shutdown timed out with " << async_io_->pending()
                      << " operation(s) still pending." << std::endl;
        }
        async_io_->shutdown();
    }
}

ResourceMetrics VirtualMachine::get_metrics() const {
    if (scheduler_) {
        return scheduler_->metrics();
    }
    return ResourceMetrics{};
}

bool VirtualMachine::save_snapshot(const std::string& snapshot_path) {
    if (!memory_) return false;

    std::ofstream out(snapshot_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    const char magic[8] = {'L', 'I', 'M', 'E', 'S', 'N', 'A', 'P'};
    out.write(magic, 8);

    uint32_t version = 1;
    out.write(reinterpret_cast<const char*>(&version), sizeof(version));

    uint32_t arch = static_cast<uint32_t>(config_.target_arch);
    out.write(reinterpret_cast<const char*>(&arch), sizeof(arch));

    uint32_t num_vcpus = static_cast<uint32_t>(vcpus_.size());
    out.write(reinterpret_cast<const char*>(&num_vcpus), sizeof(num_vcpus));

    for (const auto& vcpu : vcpus_) {
        const auto& regs = vcpu->regs();
        out.write(reinterpret_cast<const char*>(regs.data()), regs.size() * sizeof(uint64_t));

        uint64_t pc = vcpu->get_pc();
        out.write(reinterpret_cast<const char*>(&pc), sizeof(pc));

        uint32_t mode = static_cast<uint32_t>(vcpu->privilege_mode());
        out.write(reinterpret_cast<const char*>(&mode), sizeof(mode));

        uint32_t state = static_cast<uint32_t>(vcpu->state());
        out.write(reinterpret_cast<const char*>(&state), sizeof(state));

        const auto& csrs = vcpu->csrs();
        uint32_t csr_count = static_cast<uint32_t>(csrs.size());
        out.write(reinterpret_cast<const char*>(&csr_count), sizeof(csr_count));

        for (const auto& [id, val] : csrs) {
            out.write(reinterpret_cast<const char*>(&id), sizeof(id));
            out.write(reinterpret_cast<const char*>(&val), sizeof(val));
        }
    }

    // Memory pages
    uint64_t ram_cap = memory_->total_capacity_bytes();
    out.write(reinterpret_cast<const char*>(&ram_cap), sizeof(ram_cap));

    uint32_t page_sz = static_cast<uint32_t>(memory_->page_size());
    out.write(reinterpret_cast<const char*>(&page_sz), sizeof(page_sz));

    std::vector<std::pair<uint64_t, std::vector<uint8_t>>> dumped_pages;
    memory_->dump_allocated_pages(dumped_pages);

    uint64_t page_count = static_cast<uint64_t>(dumped_pages.size());
    out.write(reinterpret_cast<const char*>(&page_count), sizeof(page_count));

    for (const auto& [page_idx, data] : dumped_pages) {
        out.write(reinterpret_cast<const char*>(&page_idx), sizeof(page_idx));
        out.write(reinterpret_cast<const char*>(data.data()), data.size());
    }

    out.close();
    return true;
}

bool VirtualMachine::load_snapshot(const std::string& snapshot_path) {
    if (!memory_) return false;

    std::ifstream in(snapshot_path, std::ios::binary);
    if (!in.is_open()) return false;

    char magic[8]{};
    in.read(magic, 8);
    if (std::memcmp(magic, "LIMESNAP", 8) != 0) return false;

    uint32_t version = 0;
    in.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != 1) return false;

    uint32_t arch = 0;
    in.read(reinterpret_cast<char*>(&arch), sizeof(arch));

    uint32_t num_vcpus = 0;
    in.read(reinterpret_cast<char*>(&num_vcpus), sizeof(num_vcpus));

    for (uint32_t i = 0; i < num_vcpus; ++i) {
        std::array<uint64_t, 32> regs{};
        in.read(reinterpret_cast<char*>(regs.data()), regs.size() * sizeof(uint64_t));

        uint64_t pc = 0;
        in.read(reinterpret_cast<char*>(&pc), sizeof(pc));

        uint32_t mode = 0;
        in.read(reinterpret_cast<char*>(&mode), sizeof(mode));

        uint32_t state = 0;
        in.read(reinterpret_cast<char*>(&state), sizeof(state));

        uint32_t csr_count = 0;
        in.read(reinterpret_cast<char*>(&csr_count), sizeof(csr_count));

        std::unordered_map<uint32_t, uint64_t> csrs;
        for (uint32_t j = 0; j < csr_count; ++j) {
            uint32_t id = 0;
            uint64_t val = 0;
            in.read(reinterpret_cast<char*>(&id), sizeof(id));
            in.read(reinterpret_cast<char*>(&val), sizeof(val));
            csrs[id] = val;
        }

        if (i < vcpus_.size()) {
            vcpus_[i]->set_all_regs(regs);
            vcpus_[i]->set_pc(pc);
            vcpus_[i]->set_privilege_mode(static_cast<PrivilegeMode>(mode));
            vcpus_[i]->set_state(static_cast<VCPUState>(state));
            vcpus_[i]->set_all_csrs(csrs);
        }
    }

    uint64_t ram_cap = 0;
    in.read(reinterpret_cast<char*>(&ram_cap), sizeof(ram_cap));

    uint32_t page_sz = 0;
    in.read(reinterpret_cast<char*>(&page_sz), sizeof(page_sz));

    uint64_t page_count = 0;
    in.read(reinterpret_cast<char*>(&page_count), sizeof(page_count));

    memory_->clear_all_pages();
    for (uint64_t i = 0; i < page_count; ++i) {
        uint64_t page_idx = 0;
        in.read(reinterpret_cast<char*>(&page_idx), sizeof(page_idx));

        std::vector<uint8_t> data(page_sz);
        in.read(reinterpret_cast<char*>(data.data()), page_sz);

        memory_->restore_page(page_idx, data);
    }

    return true;
}

std::string VirtualMachine::generate_prometheus_metrics() const {
    std::string out;
    uint64_t total_cycles = 0;
    for (const auto& vcpu : vcpus_) {
        total_cycles += vcpu->total_cycles();
    }
    out += "# HELP lime_cpu_cycles_total Total virtual CPU cycles executed\n";
    out += "# TYPE lime_cpu_cycles_total counter\n";
    out += "lime_cpu_cycles_total " + std::to_string(total_cycles) + "\n";

    size_t jit_blocks = 0;
    if (vcpu_ && vcpu_->native_jit()) {
        jit_blocks = vcpu_->native_jit()->block_count();
    }
    out += "# HELP lime_jit_blocks_total Total compiled native JIT basic blocks\n";
    out += "# TYPE lime_jit_blocks_total counter\n";
    out += "lime_jit_blocks_total " + std::to_string(jit_blocks) + "\n";

    size_t mem_alloc = memory_ ? memory_->allocated_bytes() : 0;
    out += "# HELP lime_memory_allocated_bytes Allocated physical RAM in bytes\n";
    out += "# TYPE lime_memory_allocated_bytes gauge\n";
    out += "lime_memory_allocated_bytes " + std::to_string(mem_alloc) + "\n";

    size_t mem_cap = memory_ ? memory_->total_capacity_bytes() : 0;
    out += "# HELP lime_memory_capacity_bytes Total RAM capacity in bytes\n";
    out += "# TYPE lime_memory_capacity_bytes gauge\n";
    out += "lime_memory_capacity_bytes " + std::to_string(mem_cap) + "\n";

    uint64_t disk_cap = active_disk_ ? active_disk_->capacity_bytes() : 0;
    out += "# HELP lime_disk_capacity_bytes Active virtual disk capacity in bytes\n";
    out += "# TYPE lime_disk_capacity_bytes gauge\n";
    out += "lime_disk_capacity_bytes " + std::to_string(disk_cap) + "\n";

    uint64_t pkts_sent = net_device_ ? net_device_->packets_sent() : 0;
    out += "# HELP lime_net_packets_sent_total Packets sent by virtual network\n";
    out += "# TYPE lime_net_packets_sent_total counter\n";
    out += "lime_net_packets_sent_total " + std::to_string(pkts_sent) + "\n";

    uint64_t pkts_recv = net_device_ ? net_device_->packets_recv() : 0;
    out += "# HELP lime_net_packets_recv_total Packets received by virtual network\n";
    out += "# TYPE lime_net_packets_recv_total counter\n";
    out += "lime_net_packets_recv_total " + std::to_string(pkts_recv) + "\n";

    return out;
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
std::shared_ptr<AsyncIOEngine> VirtualMachine::async_io() const { return async_io_; }
std::shared_ptr<GDBStub> VirtualMachine::gdb_stub() const { return gdb_stub_; }
const VMConfig& VirtualMachine::config() const { return config_; }

std::vector<std::shared_ptr<VCPU>> VirtualMachine::vcpus() const { return vcpus_; }

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
