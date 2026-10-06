#ifndef LIME_MMU_HPP
#define LIME_MMU_HPP

#include <cstdint>
#include <memory>
#include <vector>
#include "lime/memory.hpp"

namespace lime {

enum class PrivilegeMode {
    USER = 0,
    SUPERVISOR = 1,
    MACHINE = 3
};

enum class AccessType {
    FETCH,
    READ,
    WRITE
};

struct PageFaultInfo {
    uint64_t address;
    AccessType access_type;
    PrivilegeMode mode;
    bool present;
    bool writeable;
    bool user_access;
    bool execute_disable;
};

class MMU {
public:
    explicit MMU(std::shared_ptr<MemoryManager> mem);
    ~MMU() = default;

    uint64_t translate(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t satp, bool& page_fault);
    uint64_t translate_x86_64(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t cr3, bool& page_fault);
    bool walk_page_tables(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t satp, PageFaultInfo& fault_info);
    bool is_page_present(uint64_t pa);
    bool is_write_allowed(uint64_t pa);
    bool is_user_access_allowed(uint64_t pa);
    bool is_execute_allowed(uint64_t pa);

    // TLB Cache
    void invalidate_tlb(uint64_t va = 0); // va=0 means full flush

private:
    bool walk_sv39(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t root_pt_gpa, uint64_t& out_pa, PageFaultInfo& fault_info);
    bool walk_x86_4level(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t cr3, uint64_t& out_pa, PageFaultInfo& fault_info);
    bool walk_x86_5level(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t cr3, uint64_t& out_pa, PageFaultInfo& fault_info);
    bool check_page_fault(uint64_t pa, AccessType access, PrivilegeMode mode);
    uint64_t calculate_pa(uint64_t gpa);
    bool is_large_page(uint64_t pdpte);
    uint64_t get_physical_address(const std::vector<uint64_t>& page_table_indices, int level);

    std::shared_ptr<MemoryManager> mem_;
    mutable std::mutex translate_mutex_;

    // TLB Cache (per-CPU, simple LRU)
    struct TLBEntry {
        uint64_t va;
        uint64_t pa;
        AccessType access;
        PrivilegeMode mode;
    };
    static constexpr size_t TLB_SIZE = 64;
    std::array<TLBEntry, TLB_SIZE> tlb_;
    size_t tlb_index_{0};
};

}

#endif
