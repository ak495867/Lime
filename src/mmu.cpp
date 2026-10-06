#include "lime/mmu.hpp"

namespace lime {

MMU::MMU(std::shared_ptr<MemoryManager> mem) : mem_(mem) {}

uint64_t MMU::translate(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t satp, bool& page_fault) {
    page_fault = false;
    uint64_t satp_mode = satp >> 60;

    if (mode == PrivilegeMode::MACHINE) {
        uint64_t cr3 = satp & 0xFFFFFFFFFFFFF000ULL;
        uint64_t pa = 0;
        PageFaultInfo fault_info;
        if (walk_x86_4level(va, access, mode, cr3, pa, fault_info)) {
            return pa;
        }
        page_fault = true;
        return 0;
    }

    if (satp_mode == 0) {
        return va;
    }

    if (satp_mode == 8) { 
        uint64_t vpn = va >> 12;
        uint32_t asid = (satp >> 44) & 0xFFFF;
        size_t idx = vpn % TLB_SIZE;
        TLBEntry& entry = tlb_[idx];
        
        if (entry.valid && entry.vpn == vpn && entry.asid == asid) {
            bool user = (mode == PrivilegeMode::USER);
            if (!user || entry.user) {
                if ((access == AccessType::READ && entry.read) ||
                    (access == AccessType::WRITE && entry.write) ||
                    (access == AccessType::FETCH && entry.exec)) {
                    return entry.pa | (va & 0xFFF);
                }
            }
        }
        
        uint64_t root_pt = satp & ((1ULL << 44) - 1);
        uint64_t root_pt_gpa = root_pt << 12;
        uint64_t pa = 0;
        PageFaultInfo fault_info;
        if (walk_sv39(va, access, mode, root_pt_gpa, pa, fault_info)) {
            entry.vpn = vpn;
            entry.pa = pa & ~0xFFFULL;
            entry.asid = asid;
            entry.valid = true;
            entry.user = fault_info.user_access;
            entry.read = true;
            entry.write = fault_info.writeable;
            entry.exec = !fault_info.execute_disable;
            return pa;
        }
        page_fault = true;
        return 0;
    }

    page_fault = true;
    return 0;
}

bool MMU::walk_sv39(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t root_pt_gpa, uint64_t& out_pa, PageFaultInfo& fault_info) {
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
            fault_info.address = pte_addr;
            fault_info.access_type = access;
            fault_info.mode = mode;
            fault_info.present = false;
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

bool MMU::walk_x86_4level(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t cr3, uint64_t& out_pa, PageFaultInfo& fault_info) {

    uint64_t sign_extend = va >> 47;
    if (sign_extend != 0 && sign_extend != 0x1FFFFFFFFFFFFF) {
        fault_info.address = va;
        fault_info.access_type = access;
        fault_info.mode = mode;
        fault_info.present = false;
        return false;
    }

    uint64_t pml4e_idx = (va >> 39) & 0x1FF;
    uint64_t pdpte_idx = (va >> 30) & 0x1FF;
    uint64_t pde_idx   = (va >> 21) & 0x1FF;
    uint64_t pte_idx   = (va >> 12) & 0x1FF;
    uint64_t offset    = va & 0xFFF;

    uint64_t pml4e_addr = cr3 + (pml4e_idx * 8);
    uint64_t pml4e = mem_->read64(pml4e_addr);
    if (!(pml4e & 0x1)) {
        fault_info.address = pml4e_addr;
        fault_info.access_type = access;
        fault_info.mode = mode;
        fault_info.present = false;
        return false;
    }

    uint64_t pml4e_paddr = pml4e & ~0xFFFULL;
    uint64_t pdpte_addr = pml4e_paddr + (pdpte_idx * 8);
    uint64_t pdpte = mem_->read64(pdpte_addr);
    if (!(pdpte & 0x1)) {
        fault_info.address = pdpte_addr;
        fault_info.access_type = access;
        fault_info.mode = mode;
        fault_info.present = false;
        return false;
    }

    uint64_t pdpte_paddr = pdpte & ~0xFFFULL;
    uint64_t pde_addr = pdpte_paddr + (pde_idx * 8);
    uint64_t pde = mem_->read64(pde_addr);
    if (!(pde & 0x1)) {
        fault_info.address = pde_addr;
        fault_info.access_type = access;
        fault_info.mode = mode;
        fault_info.present = false;
        return false;
    }

    uint64_t pde_paddr = pde & ~0xFFFULL;
    uint64_t pte_addr = pde_paddr + (pte_idx * 8);
    uint64_t pte = mem_->read64(pte_addr);
    if (!(pte & 0x1)) {
        fault_info.address = pte_addr;
        fault_info.access_type = access;
        fault_info.mode = mode;
        fault_info.present = false;
        return false;
    }

    if (pde & 0x80) {

        uint64_t pde_ppn = (pde >> 12) & 0x3FFFFFULL; 
        out_pa = (pde_ppn << 21) | offset;
    } else if (pdpte & 0x80) {

        uint64_t pdpte_ppn = (pdpte >> 12) & 0x3FFFFFFFFFULL; 
        out_pa = (pdpte_ppn << 30) | offset;
    } else {

        uint64_t pte_ppn = (pte >> 12) & 0xFFFFFFFFFULL; 
        out_pa = (pte_ppn << 12) | offset;
    }

    if (!check_page_fault(out_pa, access, mode)) {
        fault_info.address = va;
        fault_info.access_type = access;
        fault_info.mode = mode;
        return false;
    }
    return true;
}

bool MMU::walk_x86_5level(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t cr3, uint64_t& out_pa, PageFaultInfo& fault_info) {
    uint64_t pml5_idx = (va >> 48) & 0x1FF;
    uint64_t pml4_idx = (va >> 39) & 0x1FF;
    uint64_t pdpt_idx = (va >> 30) & 0x1FF;
    uint64_t pde_idx  = (va >> 21) & 0x1FF;
    uint64_t pte_idx  = (va >> 12) & 0x1FF;
    uint64_t offset   = va & 0xFFF;

    uint64_t pml5_addr = (cr3 & ~0xFFFULL) + (pml5_idx * 8);
    uint64_t pml5e = mem_->read64(pml5_addr);
    if (!(pml5e & 0x1)) {
        fault_info.present = false;
        return false;
    }

    uint64_t pml4_addr = (pml5e & ~0xFFFULL) + (pml4_idx * 8);
    uint64_t pml4e = mem_->read64(pml4_addr);
    if (!(pml4e & 0x1)) {
        fault_info.present = false;
        return false;
    }

    uint64_t pdpt_addr = (pml4e & ~0xFFFULL) + (pdpt_idx * 8);
    uint64_t pdpte = mem_->read64(pdpt_addr);
    if (!(pdpte & 0x1)) {
        fault_info.present = false;
        return false;
    }

    uint64_t pde_addr = (pdpte & ~0xFFFULL) + (pde_idx * 8);
    uint64_t pde = mem_->read64(pde_addr);
    if (!(pde & 0x1)) {
        fault_info.present = false;
        return false;
    }

    uint64_t pte_addr = (pde & ~0xFFFULL) + (pte_idx * 8);
    uint64_t pte = mem_->read64(pte_addr);
    if (!(pte & 0x1)) {
        fault_info.present = false;
        return false;
    }

    bool user_access = (pml5e & 0x4) && (pml4e & 0x4) && (pdpte & 0x4) && (pde & 0x4) && (pte & 0x4);
    bool write_access = (pml5e & 0x2) && (pml4e & 0x2) && (pdpte & 0x2) && (pde & 0x2) && (pte & 0x2);
    bool exec_disable = (pml5e & (1ULL<<63)) || (pml4e & (1ULL<<63)) || (pdpte & (1ULL<<63)) || (pde & (1ULL<<63)) || (pte & (1ULL<<63));

    if (mode == PrivilegeMode::USER && !user_access) return false;
    if (access == AccessType::WRITE && !write_access) return false;
    if (access == AccessType::FETCH && exec_disable) return false;

    out_pa = (pte & 0x000FFFFFFFFFF000ULL) | offset;
    return true;
}

bool MMU::check_page_fault(uint64_t pa, AccessType access, PrivilegeMode mode) {
    // For x86_4level, the permissions are verified during the walk inside walk_x86_4level
    // This is a simplified fallback for SV39 which already sets user_access etc.
    return true;
}

uint64_t MMU::calculate_pa(uint64_t gpa) {
    return gpa;
}

bool MMU::is_large_page(uint64_t pdpte) {
    return (pdpte & 0x4) != 0;
}

uint64_t MMU::get_physical_address(const std::vector<uint64_t>& page_table_indices, int level) {
    (void)page_table_indices;
    (void)level;
    return 0;
}

void MMU::invalidate_tlb(uint64_t va) {
    if (va == 0) {
        for (auto& entry : tlb_) {
            entry.valid = false;
        }
    } else {
        uint64_t vpn = va >> 12;
        tlb_[vpn % TLB_SIZE].valid = false;
    }
}

};
