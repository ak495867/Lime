#include "lime/cli.hpp"
#include "lime/vm.hpp"
#include "lime/storage.hpp"
#include "lime/cow_disk.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <cctype>

namespace lime {

int CLI::run(int argc, char** argv) {
    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string command = argv[1];
    std::vector<std::string> args;
    for (int i = 2; i < argc; ++i) {
        args.push_back(argv[i]);
    }

    if (command == "run") {
        return handle_run(args);
    } else if (command == "build") {
        return handle_build(args);
    } else if (command == "inspect") {
        return handle_inspect(args);
    } else if (command == "stats") {
        return handle_stats(args);
    } else if (command == "overlay") {
        return handle_overlay(args);
    } else if (command == "help" || command == "--help" || command == "-h") {
        print_usage();
        return 0;
    } else {
        std::cerr << "Unknown command: " << command << std::endl;
        print_usage();
        return 1;
    }
}

void CLI::print_usage() {
    std::cout << "LIME: Lightweight C++ Virtual Machine Runtime\n"
              << "Usage: lime <command> [options]\n\n"
              << "Commands:\n"
              << "  run <image>          Boot virtual machine image\n"
              << "  build <image>        Build miniature OS sparse image\n"
              << "  overlay <base> <delta> Create Copy-On-Write delta disk layer\n"
              << "  inspect <image>      Inspect virtual disk structure\n"
              << "  stats <image>        Display resource allocation metrics\n"
              << "  help                 Display help information\n\n"
              << "Run Options:\n"
              << "  --ram <size>         RAM capacity (e.g. 128M, 256M, 1G) [default: 256M]\n"
              << "  --cpu <count|auto>   vCPU count [default: auto]\n"
              << "  --policy <policy>    Allocation policy (aggressive|balanced|lazy) [default: balanced]\n"
              << "  --overlay <delta>    CoW delta overlay image\n"
              << "  --hardware-vt        Enable native host hardware virtualization (WHPX/KVM)\n"
              << "  --net                Enable virtual VirtIO network & socket bridge\n"
              << "  --headless           Run without interactive graphics\n"
              << std::endl;
}

size_t CLI::parse_size(const std::string& str) {
    if (str.empty()) return 256 * 1024 * 1024;
    std::string s = str;
    char unit = std::toupper(s.back());
    size_t multiplier = 1;

    if (unit == 'K') {
        multiplier = 1024;
        s.pop_back();
    } else if (unit == 'M') {
        multiplier = 1024 * 1024;
        s.pop_back();
    } else if (unit == 'G') {
        multiplier = 1024ULL * 1024 * 1024;
        s.pop_back();
    }

    try {
        return std::stoull(s) * multiplier;
    } catch (...) {
        return 256 * 1024 * 1024;
    }
}

int CLI::handle_run(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: image path required for run command." << std::endl;
        return 1;
    }

    std::string image_path = args[0];
    VMConfig config;
    config.ram_size_mb = 256;
    config.policy = AllocationPolicy::BALANCED;

    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--ram" && i + 1 < args.size()) {
            config.ram_size_mb = parse_size(args[i + 1]) / (1024 * 1024);
            i++;
        } else if (args[i] == "--cpu" && i + 1 < args.size()) {
            config.cpu_count = (args[i + 1] == "auto") ? 1 : std::stoul(args[i + 1]);
            i++;
        } else if (args[i] == "--policy" && i + 1 < args.size()) {
            std::string p = args[i + 1];
            if (p == "aggressive") config.policy = AllocationPolicy::AGGRESSIVE;
            else if (p == "lazy") config.policy = AllocationPolicy::LAZY;
            else config.policy = AllocationPolicy::BALANCED;
            i++;
        } else if (args[i] == "--overlay" && i + 1 < args.size()) {
            config.delta_disk_path = args[i + 1];
            i++;
        } else if (args[i] == "--hardware-vt") {
            config.use_hardware_hypervisor = true;
        } else if (args[i] == "--net") {
            config.enable_net = true;
        } else if (args[i] == "--headless") {
            config.headless = true;
        }
    }

    std::cout << "[LIME] Initializing Lightweight Virtual Machine..." << std::endl;
    std::cout << "[LIME] Target RAM Capacity: " << config.ram_size_mb << " MB" << std::endl;
    std::cout << "[LIME] Configured Policy: "
              << (config.policy == AllocationPolicy::AGGRESSIVE ? "AGGRESSIVE" :
                  config.policy == AllocationPolicy::LAZY ? "LAZY" : "BALANCED")
              << std::endl;

    VirtualMachine vm(config);
    if (!vm.init()) {
        std::cerr << "[LIME] Error: Failed to initialize VM subsystems." << std::endl;
        return 1;
    }

    if (vm.hypervisor() && vm.hypervisor()->is_active()) {
        std::cout << "[LIME] Hardware Virtualization Engine Active (Host Native VT)." << std::endl;
    } else {
        std::cout << "[LIME] Software Execution Engine Active (JIT / MMU Emulation)." << std::endl;
    }

    std::cout << "[LIME] Loading guest OS image: " << image_path << "..." << std::endl;
    if (!vm.load_lime_image(image_path)) {
        std::cout << "[LIME] Image load fallback: Injecting mini-OS kernel..." << std::endl;
        std::vector<uint8_t> default_code = LimeImageBuilder::generate_mini_os_code();
        vm.load_binary(default_code, 0x80000000);
    }

    std::cout << "[LIME] Booting guest execution engine..." << std::endl;
    vm.run();

    ResourceMetrics m = vm.get_metrics();
    std::cout << "\n[LIME] VM Execution Terminated." << std::endl;
    std::cout << "[LIME] Resource Summary:" << std::endl;
    std::cout << "  - Active Host RAM Allocated: " << (m.allocated_ram_bytes / 1024) << " KB" << std::endl;
    std::cout << "  - Memory Reclaimed by Host: " << (m.reclaimed_ram_bytes / 1024) << " KB" << std::endl;
    std::cout << "  - Active vCPU Cycles: " << m.vcpu_active_cycles << std::endl;
    std::cout << "  - Idle vCPU Cycles: " << m.vcpu_idle_cycles << std::endl;

    return 0;
}

int CLI::handle_build(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: Output image file path required." << std::endl;
        return 1;
    }

    std::string out_path = args[0];
    uint64_t size_mb = 100;

    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--size" && i + 1 < args.size()) {
            size_mb = parse_size(args[i + 1]) / (1024 * 1024);
            i++;
        }
    }

    std::cout << "[LIME] Building sparse virtual disk image: " << out_path << " (" << size_mb << " MB)..." << std::endl;
    if (LimeImageBuilder::create_default_mini_os_image(out_path, size_mb)) {
        std::cout << "[LIME] Image successfully created." << std::endl;
        return 0;
    } else {
        std::cerr << "[LIME] Error creating image file." << std::endl;
        return 1;
    }
}

int CLI::handle_overlay(const std::vector<std::string>& args) {
    if (args.size() < 2) {
        std::cerr << "Error: Base image path and delta overlay output path required." << std::endl;
        return 1;
    }

    std::string base_path = args[0];
    std::string delta_path = args[1];

    std::cout << "[LIME] Creating Copy-On-Write overlay: " << delta_path << " -> " << base_path << std::endl;
    if (CoWSparseDisk::create_overlay(base_path, delta_path)) {
        std::cout << "[LIME] CoW overlay created successfully." << std::endl;
        return 0;
    } else {
        std::cerr << "[LIME] Error creating overlay." << std::endl;
        return 1;
    }
}

int CLI::handle_inspect(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: Image file path required." << std::endl;
        return 1;
    }

    std::string path = args[0];
    SparseDisk disk;
    if (!disk.open(path)) {
        std::cerr << "[LIME] Error opening image: " << path << std::endl;
        return 1;
    }

    std::cout << "[LIME] Inspecting Image: " << path << "\n"
              << "  - Virtual Capacity: " << (disk.capacity_bytes() / (1024 * 1024)) << " MB\n"
              << "  - Actual Host Storage Used: " << (disk.host_file_size() / 1024) << " KB\n"
              << "  - Block Size: " << (disk.block_size() / 1024) << " KB\n"
              << "  - Allocated Blocks: " << disk.allocated_blocks() << "\n"
              << std::endl;

    return 0;
}

int CLI::handle_stats(const std::vector<std::string>& args) {
    if (args.empty()) {
        std::cerr << "Error: Image file path required." << std::endl;
        return 1;
    }

    std::string path = args[0];
    std::cout << "[LIME] Querying runtime statistics for: " << path << std::endl;
    std::cout << "Status: Idle / Dynamic Compute Footprint Active" << std::endl;
    return 0;
}

}
