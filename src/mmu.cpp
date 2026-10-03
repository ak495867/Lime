#include "lime/mmu.hpp"

namespace lime {

MMU::MMU(std::shared_ptr<MemoryManager> mem) : mem_(mem) {}

uint64_t MMU::translate(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t satp, bool& page_fault) {
    page_fault = false;
    uint64_t satp_mode = satp >> 60;
    if (mode == PrivilegeMode::MACHINE || satp_mode == 0) {
        return va;
    }

    if (satp_mode == 8) {
        uint64_t root_pt_gpa = (satp & 0x000FFFFFFFFFFFFFULL) << 12;
        uint64_t pa = 0;
        if (walk_sv39(va, access, mode, root_pt_gpa, pa)) {
            return pa;
        }
    }

    page_fault = true;
    return 0;
}

bool MMU::walk_sv39(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t root_pt_gpa, uint64_t& out_pa) {
    uint64_t vpn[3] = {
        (va >> 12) & 0x1FF,
        (va >> 21) & 0x1FF,
        (va >> 30) & 0x1FF
    };
    uint64_t page_offset = va & 0xFFF;

    uint64_t a = root_pt_gpa;
    for (int i = 2; i >= 0; --i) {
        uint64_t pte_addr = a + vpn[i] * 8;
        uint64_t pte = mem_->read64(pte_addr);

        bool v = (pte & 1);
        bool r = (pte & 2);
        bool w = (pte & 4);
        bool x = (pte & 8);

        if (!v || (!r && w)) {
            return false;
        }

        if (r || x) {
            if (access == AccessType::FETCH && !x) return false;
            if (access == AccessType::READ && !r) return false;
            if (access == AccessType::WRITE && !w) return false;

            bool u = (pte & 0x10);
            if (mode == PrivilegeMode::USER && !u) return false;
            if (mode == PrivilegeMode::SUPERVISOR && u) return false;

            uint64_t ppn = (pte >> 10) & 0x000FFFFFFFFFFFFFULL;
            out_pa = (ppn << 12) | page_offset;
            return true;
        }

        a = ((pte >> 10) & 0x000FFFFFFFFFFFFFULL) << 12;
    }

    return false;
}

}
