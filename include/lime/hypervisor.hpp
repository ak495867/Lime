#ifndef LIME_HYPERVISOR_HPP
#define LIME_HYPERVISOR_HPP

#include <cstdint>
#include <memory>
#include <vector>
#include <string>

namespace lime {

enum class HypervisorType {
    SOFTWARE_FALLBACK,
    WHPX_WINDOWS,
    KVM_LINUX
};

struct HypervisorCapabilities {
    bool supported{false};
    HypervisorType type{HypervisorType::SOFTWARE_FALLBACK};
    uint32_t max_vcpus{16};
    bool nested_paging{false};
};

class HostHypervisor {
public:
    static HypervisorCapabilities detect();

    HostHypervisor();
    ~HostHypervisor();

    bool initialize(HypervisorType preferred_type = HypervisorType::WHPX_WINDOWS);
    void shutdown();

    bool create_vm();
    bool map_guest_memory(uint64_t gpa, void* host_address, size_t size_bytes, bool read, bool write, bool execute);
    bool unmap_guest_memory(uint64_t gpa, size_t size_bytes);

    bool create_vcpu(uint32_t vcpu_id);
    bool run_vcpu(uint32_t vcpu_id);
    bool interrupt_vcpu(uint32_t vcpu_id);
    bool create_nested_vm();
    bool create_nested_vcpu(uint32_t vcpu_id);

    HypervisorType type() const;
    bool is_active() const;

private:
    HypervisorCapabilities caps_;
    bool is_active_{false};
    void* handle_{nullptr};
    bool nested_enabled_{false};
};

}

#endif
