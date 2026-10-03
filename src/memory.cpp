#include "lime/memory.hpp"
#include <cstring>
#include <algorithm>

namespace lime {

MemoryManager::MemoryManager(size_t total_ram_bytes, size_t page_size)
    : total_ram_bytes_(total_ram_bytes), page_size_(page_size) {
    total_pages_ = (total_ram_bytes + page_size - 1) / page_size;
}

uint8_t* MemoryManager::ensure_page(uint64_t page_index, bool is_write) {
    if (pages_.size() >= total_pages_ && pages_.find(page_index) == pages_.end()) {
        return nullptr;
    }
    access_counter_++;
    auto it = pages_.find(page_index);
    if (it == pages_.end()) {
        fault_count_++;
        std::vector<uint8_t> new_page(page_size_, 0);
        auto insert_res = pages_.emplace(page_index, std::move(new_page));
        it = insert_res.first;
        PageInfo info;
        info.allocated = true;
        info.dirty = is_write;
        info.last_accessed = access_counter_;
        info.ballooned = false;
        page_table_[page_index] = info;
    } else {
        auto& info = page_table_[page_index];
        if (info.ballooned) {
            info.ballooned = false;
            if (reclaimed_bytes_ >= page_size_) {
                reclaimed_bytes_ -= page_size_;
            } else {
                reclaimed_bytes_ = 0;
            }
        }
        info.last_accessed = access_counter_;
        if (is_write) {
            info.dirty = true;
        }
    }
    return it->second.data();
}

uint8_t MemoryManager::read8(uint64_t gpa) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t page_idx = gpa / page_size_;
    uint64_t offset = gpa % page_size_;
    uint8_t* page = ensure_page(page_idx, false);
    if (!page) return 0;
    return page[offset];
}

uint16_t MemoryManager::read16(uint64_t gpa) {
    uint16_t val = 0;
    read_bytes(gpa, &val, sizeof(val));
    return val;
}

uint32_t MemoryManager::read32(uint64_t gpa) {
    uint32_t val = 0;
    read_bytes(gpa, &val, sizeof(val));
    return val;
}

uint64_t MemoryManager::read64(uint64_t gpa) {
    uint64_t val = 0;
    read_bytes(gpa, &val, sizeof(val));
    return val;
}

void MemoryManager::write8(uint64_t gpa, uint8_t val) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t page_idx = gpa / page_size_;
    uint64_t offset = gpa % page_size_;
    uint8_t* page = ensure_page(page_idx, true);
    if (page) {
        page[offset] = val;
    }
}

void MemoryManager::write16(uint64_t gpa, uint16_t val) {
    write_bytes(gpa, &val, sizeof(val));
}

void MemoryManager::write32(uint64_t gpa, uint32_t val) {
    write_bytes(gpa, &val, sizeof(val));
}

void MemoryManager::write64(uint64_t gpa, uint64_t val) {
    write_bytes(gpa, &val, sizeof(val));
}

bool MemoryManager::read_bytes(uint64_t gpa, void* dst, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint8_t* out = static_cast<uint8_t*>(dst);
    size_t remaining = len;
    uint64_t curr_gpa = gpa;

    while (remaining > 0) {
        uint64_t page_idx = curr_gpa / page_size_;
        uint64_t offset = curr_gpa % page_size_;
        size_t chunk = std::min(remaining, page_size_ - offset);

        uint8_t* page = ensure_page(page_idx, false);
        if (!page) return false;

        std::memcpy(out, page + offset, chunk);
        out += chunk;
        curr_gpa += chunk;
        remaining -= chunk;
    }
    return true;
}

bool MemoryManager::write_bytes(uint64_t gpa, const void* src, size_t len) {
    std::lock_guard<std::mutex> lock(mutex_);
    const uint8_t* in = static_cast<const uint8_t*>(src);
    size_t remaining = len;
    uint64_t curr_gpa = gpa;

    while (remaining > 0) {
        uint64_t page_idx = curr_gpa / page_size_;
        uint64_t offset = curr_gpa % page_size_;
        size_t chunk = std::min(remaining, page_size_ - offset);

        uint8_t* page = ensure_page(page_idx, true);
        if (!page) return false;

        std::memcpy(page + offset, in, chunk);
        in += chunk;
        curr_gpa += chunk;
        remaining -= chunk;
    }
    return true;
}

size_t MemoryManager::inflate_balloon(size_t target_pages) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t inflated = 0;
    for (auto& pair : page_table_) {
        if (inflated >= target_pages) break;
        if (pair.second.allocated && !pair.second.ballooned) {
            pair.second.ballooned = true;
            pages_.erase(pair.first);
            reclaimed_bytes_ += page_size_;
            inflated++;
        }
    }
    return inflated;
}

size_t MemoryManager::deflate_balloon(size_t target_pages) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t deflated = 0;
    for (auto& pair : page_table_) {
        if (deflated >= target_pages) break;
        if (pair.second.ballooned) {
            pair.second.ballooned = false;
            pages_[pair.first] = std::vector<uint8_t>(page_size_, 0);
            if (reclaimed_bytes_ >= page_size_) {
                reclaimed_bytes_ -= page_size_;
            } else {
                reclaimed_bytes_ = 0;
            }
            deflated++;
        }
    }
    return deflated;
}

size_t MemoryManager::reclaim_idle_pages(uint64_t current_tick, uint64_t max_idle_duration) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t reclaimed = 0;
    for (auto& pair : page_table_) {
        if (pair.second.allocated && !pair.second.ballooned) {
            if (current_tick > pair.second.last_accessed &&
                (current_tick - pair.second.last_accessed) > max_idle_duration) {
                pair.second.ballooned = true;
                pages_.erase(pair.first);
                reclaimed_bytes_ += page_size_;
                reclaimed++;
            }
        }
    }
    return reclaimed;
}

size_t MemoryManager::allocated_bytes() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pages_.size() * page_size_;
}

size_t MemoryManager::total_capacity_bytes() const {
    return total_ram_bytes_;
}

size_t MemoryManager::reclaimed_bytes() const {
    return reclaimed_bytes_.load();
}

uint64_t MemoryManager::fault_count() const {
    return fault_count_.load();
}

size_t MemoryManager::page_size() const {
    return page_size_;
}

}
