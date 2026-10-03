#ifndef LIME_MEMORY_HPP
#define LIME_MEMORY_HPP

#include <cstdint>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <cstddef>

namespace lime {

struct PageInfo {
    bool allocated{false};
    bool dirty{false};
    uint64_t last_accessed{0};
    bool ballooned{false};
};

class MemoryManager {
public:
    explicit MemoryManager(size_t total_ram_bytes, size_t page_size = 4096);
    ~MemoryManager() = default;

    uint8_t read8(uint64_t gpa);
    uint16_t read16(uint64_t gpa);
    uint32_t read32(uint64_t gpa);
    uint64_t read64(uint64_t gpa);

    void write8(uint64_t gpa, uint8_t val);
    void write16(uint64_t gpa, uint16_t val);
    void write32(uint64_t gpa, uint32_t val);
    void write64(uint64_t gpa, uint64_t val);

    bool read_bytes(uint64_t gpa, void* dst, size_t len);
    bool write_bytes(uint64_t gpa, const void* src, size_t len);

    size_t inflate_balloon(size_t target_pages);
    size_t deflate_balloon(size_t target_pages);
    size_t reclaim_idle_pages(uint64_t current_tick, uint64_t max_idle_duration);

    size_t allocated_bytes() const;
    size_t total_capacity_bytes() const;
    size_t reclaimed_bytes() const;
    uint64_t fault_count() const;
    size_t page_size() const;

private:
    uint8_t* ensure_page(uint64_t page_index, bool is_write);

    size_t total_ram_bytes_;
    size_t page_size_;
    size_t total_pages_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> pages_;
    std::unordered_map<uint64_t, PageInfo> page_table_;
    std::atomic<uint64_t> fault_count_{0};
    std::atomic<size_t> reclaimed_bytes_{0};
    uint64_t access_counter_{0};
};

}

#endif
