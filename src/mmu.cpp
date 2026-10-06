#include "lime/mmu.hpp"

namespace lime {

MMU::MMU(std::shared_ptr<MemoryManager> mem) : mem_(mem) {}

uint64_t MMU::translate(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t satp, bool& page_fault) {
    page_fault = false;
    uint64_t satp_mode = satp >> 60;

    if (mode == PrivilegeMode::MACHINE) {
        // x86_64 mode - CR3 drives translation (bit 63 set in satp as CR3 indicator)
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
        // No paging - identity mapping
        return va;
    }

    if (satp_mode == 8) { // Sv39 (RISC-V 4-level)
        uint64_t root_pt = satp & ((1ULL << 44) - 1);
        uint64_t root_pt_gpa = root_pt << 12;
        uint64_t pa = 0;
        PageFaultInfo fault_info;
        if (walk_sv39(va, access, mode, root_pt_gpa, pa, fault_info)) {
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
    // Validate canonical address (x86-64 uses 48-bit addresses)
    uint64_t sign_extend = va >> 47;
    if (sign_extend != 0 && sign_extend != 0x1FFFFFFFFFFFFF) {
        fault_info.address = va;
        fault_info.access_type = access;
        fault_info.mode = mode;
        fault_info.present = false;
        return false;
    }

    // Extract the four level indices and page offset
    uint64_t pml4e_idx = (va >> 39) & 0x1FF;
    uint64_t pdpte_idx = (va >> 30) & 0x1FF;
    uint64_t pde_idx   = (va >> 21) & 0x1FF;
    uint64_t pte_idx   = (va >> 12) & 0x1FF;
    uint64_t offset    = va & 0xFFF;

    // --- Level 1: PML4E ---
    uint64_t pml4e_addr = cr3 + (pml4e_idx * 8);
    uint64_t pml4e = mem_->read64(pml4e_addr);
    if (!(pml4e & 0x1)) {
        fault_info.address = pml4e_addr;
        fault_info.access_type = access;
        fault_info.mode = mode;
        fault_info.present = false;
        return false;
    }

    // --- Level 2: PDPTE ---
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

    // --- Level 3: PDE ---
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

    // --- Level 4: PTE ---
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

    // --- Build physical address based on page size ---
    if (pde & 0x80) {
        // 2MB large page (PS bit in PDE)
        uint64_t pde_ppn = (pde >> 12) & 0x3FFFFFULL; // 22-bit PPN
        out_pa = (pde_ppn << 21) | offset;
    } else if (pdpte & 0x80) {
        // 1GB large page (PS bit in PDPTE)
        uint64_t pdpte_ppn = (pdpte >> 12) & 0x3FFFFFFFFFULL; // 30-bit PPN
        out_pa = (pdpte_ppn << 30) | offset;
    } else {
        // 4KB page
        uint64_t pte_ppn = (pte >> 12) & 0xFFFFFFFFFULL; // 48-bit PPN
        out_pa = (pte_ppn << 12) | offset;
    }

    // Check access permissions
    if (!check_page_fault(out_pa, access, mode)) {
        fault_info.address = va;
        fault_info.access_type = access;
        fault_info.mode = mode;
        return false;
    }
    return true;
}

bool MMU::walk_x86_5level(uint64_t va, AccessType access, PrivilegeMode mode, uint64_t cr3, uint64_t& out_pa, PageFaultInfo& fault_info) {
    // Placeholder: 5-level paging (05H satp mode) - currently forwards to 4-level
    // Full implementation would add a level-0 walk from CR4.LA57
    return walk_x86_4level(va, access, mode, cr3, out_pa, fault_info);
}

bool MMU::check_page_fault(uint64_t pa, AccessType access, PrivilegeMode mode) {
    // Simplified permission checking - a full implementation would read
    // the PTE R/W/U/X bits and verify against the access type and privilege mode.
    (void)pa; (void)access; (void)mode;
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
    // Full TLB flush for now
    (void)va;
    tlb_.fill({0, 0, AccessType::READ, PrivilegeMode::MACHINE});
}

};