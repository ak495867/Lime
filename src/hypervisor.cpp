#include "lime/hypervisor.hpp"
#include <iostream>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace lime {

HypervisorCapabilities HostHypervisor::detect() {
    HypervisorCapabilities caps;
#if defined(_WIN32)
    HMODULE hModule = LoadLibraryA("WinHvPlatform.dll");
    if (hModule) {
        caps.supported = true;
        caps.type = HypervisorType::WHPX_WINDOWS;
        FreeLibrary(hModule);
    } else {
        caps.supported = false;
        caps.type = HypervisorType::SOFTWARE_FALLBACK;
    }
#elif defined(__linux__)
    caps.supported = true;
    caps.type = HypervisorType::KVM_LINUX;
#else
    caps.supported = false;
    caps.type = HypervisorType::SOFTWARE_FALLBACK;
#endif
    return caps;
}

HostHypervisor::HostHypervisor() {
    caps_ = detect();
}

HostHypervisor::~HostHypervisor() {
    shutdown();
}

bool HostHypervisor::initialize(HypervisorType preferred_type) {
    caps_ = detect();
    if (preferred_type != HypervisorType::SOFTWARE_FALLBACK && caps_.supported) {
        is_active_ = true;
        return true;
    }
    caps_.type = HypervisorType::SOFTWARE_FALLBACK;
    is_active_ = false;
    return true;
}

void HostHypervisor::shutdown() {
    is_active_ = false;
    handle_ = nullptr;
}

bool HostHypervisor::create_vm() {
    return true;
}

bool HostHypervisor::map_guest_memory(uint64_t, void*, size_t, bool, bool, bool) {
    return true;
}

bool HostHypervisor::unmap_guest_memory(uint64_t, size_t) {
    return true;
}

bool HostHypervisor::create_vcpu(uint32_t) {
    return true;
}

bool HostHypervisor::run_vcpu(uint32_t) {
    return true;
}

bool HostHypervisor::interrupt_vcpu(uint32_t) {
    return true;
}

HypervisorType HostHypervisor::type() const {
    return caps_.type;
}

bool HostHypervisor::is_active() const {
    return is_active_;
}

}
