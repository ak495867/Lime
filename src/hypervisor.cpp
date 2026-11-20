#include "lime/hypervisor.hpp"
#include <iostream>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
typedef HRESULT (WINAPI *WHvGetCapability_t)(int, void*, UINT32, UINT32*);
#elif defined(__linux__)
#include <unistd.h>
#include <fcntl.h>
#elif defined(__APPLE__)
#include <sys/types.h>
#include <sys/sysctl.h>
#endif

namespace lime {

HypervisorCapabilities HostHypervisor::detect() {
    HypervisorCapabilities caps;
    caps.supported = false;
    caps.type = HypervisorType::SOFTWARE_FALLBACK;
    caps.max_vcpus = 16;
    caps.nested_paging = false;

#if defined(_WIN32)
    HMODULE mod = LoadLibraryA("WinHvPlatform.dll");
    if (mod) {
        WHvGetCapability_t pfn = reinterpret_cast<WHvGetCapability_t>(GetProcAddress(mod, "WHvGetCapability"));
        if (pfn) {
            BOOL is_present = FALSE;
            UINT32 written = 0;
            // WHvCapabilityCodeHypervisorPresent = 0
            if (SUCCEEDED(pfn(0, &is_present, sizeof(is_present), &written)) && is_present) {
                caps.supported = true;
                caps.type = HypervisorType::WHPX_WINDOWS;
                caps.max_vcpus = 64;
                caps.nested_paging = true;
            }
        }
        FreeLibrary(mod);
    }
#elif defined(__linux__)
    if (access("/dev/kvm", R_OK | W_OK) == 0) {
        caps.supported = true;
        caps.type = HypervisorType::KVM_LINUX;
        caps.max_vcpus = 256;
        caps.nested_paging = true;
    }
#elif defined(__APPLE__)
    int hv_support = 0;
    size_t size = sizeof(hv_support);
    if (sysctlbyname("kern.hv_support", &hv_support, &size, NULL, 0) == 0 && hv_support == 1) {
        caps.supported = true;
        caps.type = HypervisorType::HVF_MACOS;
        caps.max_vcpus = 16;
        caps.nested_paging = true;
    }
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
    return false;
}

bool HostHypervisor::map_guest_memory(uint64_t, void*, size_t, bool, bool, bool) {
    return false;
}

bool HostHypervisor::unmap_guest_memory(uint64_t, size_t) {
    return false;
}

bool HostHypervisor::create_vcpu(uint32_t) {
    return false;
}

bool HostHypervisor::run_vcpu(uint32_t) {
    return false;
}

bool HostHypervisor::interrupt_vcpu(uint32_t) {
    return false;
}

bool HostHypervisor::create_nested_vm() {
    return false;
}

bool HostHypervisor::create_nested_vcpu(uint32_t) {
    return false;
}

HypervisorType HostHypervisor::type() const {
    return caps_.type;
}

bool HostHypervisor::is_active() const {
    return is_active_;
}

}
