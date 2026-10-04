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
    if (!is_active_) return false;
    
#if defined(_WIN32) && defined(WHPX_WINDOWS)
    // WHPX: WHvCreatePartition
    if (caps_.type == HypervisorType::WHPX_WINDOWS) {
        HRESULT hr = WHvCreatePartition(&handle_);
        return SUCCEEDED(hr);
    }
#elif defined(__linux__) && defined(KVM_LINUX)
    // KVM: ioctl(KVM_CREATE_VM)
    if (caps_.type == HypervisorType::KVM_LINUX) {
        int fd = open("/dev/kvm", O_RDWR);
        if (fd < 0) return false;
        handle_ = reinterpret_cast<void*>(static_cast<intptr_t>(fd));
        int ret = ioctl(static_cast<int>(reinterpret_cast<intptr_t>(handle_)), KVM_CREATE_VM, 0);
        if (ret < 0) {
            close(static_cast<int>(reinterpret_cast<intptr_t>(handle_)));
            handle_ = nullptr;
            return false;
        }
        return true;
    }
#endif
    return false;
}

bool HostHypervisor::map_guest_memory(uint64_t gpa, void* host_address, size_t size_bytes, bool read, bool write, bool execute) {
    if (!is_active_ || !handle_) return false;
    
#if defined(_WIN32) && defined(WHPX_WINDOWS)
    // WHPX: WHvMapGpaRange
    if (caps_.type == HypervisorType::WHPX_WINDOWS) {
        WHV_GPA_RANGE range = { gpa, size_bytes };
        WHV_MAP_GPA_RANGE_FLAGS flags = 0;
        if (read) flags |= WHV_MAP_GPA_RANGE_FLAG_READ;
        if (write) flags |= WHV_MAP_GPA_RANGE_FLAG_WRITE;
        if (execute) flags |= WHV_MAP_GPA_RANGE_FLAG_EXECUTE;
        
        HRESULT hr = WHvMapGpaRange(handle_, host_address, &range, 1, flags);
        return SUCCEEDED(hr);
    }
#elif defined(__linux__) && defined(KVM_LINUX)
    // KVM: ioctl(KVM_SET_USER_MEMORY_REGION)
    if (caps_.type == HypervisorType::KVM_LINUX) {
        struct kvm_userspace_memory_region memreg;
        memreg.slot = 0;
        memreg.guest_phys_addr = gpa;
        memreg.memory_size = size_bytes;
        memreg.userspace_addr = reinterpret_cast<uint64_t>(host_address);
        memreg.flags = 0;
        
        int ret = ioctl(static_cast<int>(reinterpret_cast<intptr_t>(handle_)), KVM_SET_USER_MEMORY_REGION, &memreg);
        return ret >= 0;
    }
#endif
    return false;
}

bool HostHypervisor::unmap_guest_memory(uint64_t gpa, size_t size_bytes) {
    if (!is_active_ || !handle_) return false;
    
#if defined(_WIN32) && defined(WHPX_WINDOWS)
    // WHPX: WHvUnmapGpaRange
    if (caps_.type == HypervisorType::WHPX_WINDOWS) {
        WHV_GPA_RANGE range = { gpa, size_bytes };
        HRESULT hr = WHvUnmapGpaRange(handle_, &range, 1);
        return SUCCEEDED(hr);
    }
#elif defined(__linux__) && defined(KVM_LINUX)
    // KVM: ioctl(KVM_SET_USER_MEMORY_REGION with size=0)
    if (caps_.type == HypervisorType::KVM_LINUX) {
        struct kvm_userspace_memory_region memreg;
        memreg.slot = 0;
        memreg.guest_phys_addr = gpa;
        memreg.memory_size = 0;  // Size 0 means unmap
        memreg.userspace_addr = 0;
        memreg.flags = 0;
        
        int ret = ioctl(static_cast<int>(reinterpret_cast<intptr_t>(handle_)), KVM_SET_USER_MEMORY_REGION, &memreg);
        return ret >= 0;
    }
#endif
    return false;
}

bool HostHypervisor::create_vcpu(uint32_t vcpu_id) {
    if (!is_active_ || !handle_) return false;
    
#if defined(_WIN32) && defined(WHPX_WINDOWS)
    // WHPX: WHvCreateVp
    if (caps_.type == HypervisorType::WHPX_WINDOWS) {
        WHV_VP_INDEX vp_index = vcpu_id;
        HRESULT hr = WHvCreateVp(handle_, vp_index);
        return SUCCEEDED(hr);
    }
#elif defined(__linux__) && defined(KVM_LINUX)
    // KVM: ioctl(KVM_CREATE_VCPU)
    if (caps_.type == HypervisorType::KVM_LINUX) {
        int ret = ioctl(static_cast<int>(reinterpret_cast<intptr_t>(handle_)), KVM_CREATE_VCPU, vcpu_id);
        return ret >= 0;
    }
#endif
    return false;
}

bool HostHypervisor::run_vcpu(uint32_t vcpu_id) {
    if (!is_active_ || !handle_) return false;
    
#if defined(_WIN32) && defined(WHPX_WINDOWS)
    // WHPX: WHvRunVp
    if (caps_.type == HypervisorType::WHPX_WINDOWS) {
        WHV_VP_INDEX vp_index = vcpu_id;
        WHV_RUN_VP_EXIT_CONTEXT exit_context;
        HRESULT hr = WHvRunVp(handle_, vp_index, &exit_context, sizeof(exit_context));
        // Handle exit reasons as needed
        return SUCCEEDED(hr);
    }
#elif defined(__linux__) && defined(KVM_LINUX)
    // KVM: ioctl(KVM_RUN)
    if (caps_.type == HypervisorType::KVM_LINUX) {
        struct kvm_run* run_state = nullptr;
        size_t mmap_size = ioctl(static_cast<int>(reinterpret_cast<intptr_t>(handle_)), KVM_GET_VCPU_MMAP_SIZE, 0);
        if (mmap_size <= 0) return false;
        run_state = static_cast<struct kvm_run*>(mmap(nullptr, mmap_size, PROT_READ | PROT_WRITE, MAP_SHARED, 
                                                      static_cast<int>(reinterpret_cast<intptr_t>(handle_)), 0));
        if (run_state == MAP_FAILED) return false;
        
        int ret = ioctl(static_cast<int>(reinterpret_cast<intptr_t>(handle_)), KVM_RUN, 0);
        munmap(run_state, mmap_size);
        return ret >= 0;
    }
#endif
    return false;
}

bool HostHypervisor::interrupt_vcpu(uint32_t vcpu_id) {
    if (!is_active_ || !handle_) return false;
    
#if defined(_WIN32) && defined(WHPX_WINDOWS)
    // WHPX: WHvSintDeliver
    if (caps_.type == HypervisorType::WHPX_WINDOWS) {
        WHV_VP_INDEX vp_index = vcpu_id;
        WHV_SINT DELIVER_PARAMETERS params = {};
        params.SintType = WhvSintTypeMessage;  // or WhvSintTypeSynthetic
        params.SintNumber = 0;  // Adjust as needed
        HRESULT hr = WHvSintDeliver(handle_, vp_index, &params);
        return SUCCEEDED(hr);
    }
#elif defined(__linux__) && defined(KVM_LINUX)
    // KVM: ioctl(KVM_IRQ_INJECT)
    if (caps_.type == HypervisorType::KVM_LINUX) {
        struct kvm_irq_level irq_event;
        irq_event.irq = 0;  // Adjust as needed
        irq_event.level = 1;  // Level-triggered interrupt
        int ret = ioctl(static_cast<int>(reinterpret_cast<intptr_t>(handle_)), KVM_IRQ_INJECT, &irq_event);
        return ret >= 0;
    }
#endif
    return false;
}

HypervisorType HostHypervisor::type() const {
    return caps_.type;
}

bool HostHypervisor::is_active() const {
    return is_active_;
}

}
