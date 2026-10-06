#include "lime/hypervisor.hpp"
#include <iostream>

namespace lime {

HypervisorCapabilities HostHypervisor::detect() {
    HypervisorCapabilities caps;
    caps.supported = false;
    caps.type = HypervisorType::SOFTWARE_FALLBACK;
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
