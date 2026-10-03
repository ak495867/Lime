#ifndef LIME_MMU_HPP
#define LIME_MMU_HPP

#include <cstdint>
#include <memory>
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

class MMU {
public:
    explicit MMU(std::shared_ptr<MemoryManager> mem);
    ~MMU() = default;

    uint64_t translate(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t satp, bool& page_fault);

private:
    bool walk_sv39(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t root_pt_gpa, uint64_t& out_pa);

    std::shared_ptr<MemoryManager> mem_;
};

}

#endif
