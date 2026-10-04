#ifndef LIME_STORAGE_HPP
#define LIME_STORAGE_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <mutex>
#include <list>
#include <unordered_map>

namespace lime {

#pragma pack(push, 1)
struct LimeSparseDiskHeader {
    char magic[4]{'L', 'I', 'M', 'E'};
    uint32_t version{1};
    uint64_t virtual_capacity_bytes{0};
    uint32_t block_size{65536};
    uint32_t total_blocks{0};
    uint32_t allocated_blocks{0};
};
#pragma pack(pop)

class SparseDisk {
public:
    static const uint32_t SECTOR_SIZE = 512;

    SparseDisk();
    ~SparseDisk();

    static bool create(const std::string& path, uint64_t capacity_bytes, uint32_t block_size = 65536);
    bool open(const std::string& path);
    void close();

    bool read_sectors(uint64_t lba, uint32_t sector_count, void* buffer);
    bool write_sectors(uint64_t lba, uint32_t sector_count, const void* buffer);

    uint64_t capacity_bytes() const;
    uint64_t host_file_size() const;
    uint32_t block_size() const;
    uint32_t allocated_blocks() const;
    bool is_open() const;

private:
    uint64_t get_block_file_offset(uint32_t block_index);
    uint32_t allocate_block(uint32_t block_index);

    std::string file_path_;
    mutable std::fstream file_;
    LimeSparseDiskHeader header_;
    std::vector<uint32_t> block_table_;
    mutable std::mutex mutex_;
    bool is_open_{false};

    // LRU cache for frequently accessed blocks
    static constexpr size_t LRU_CACHE_SIZE = 1024; // Cache up to 1024 blocks (64MB with 64KB blocks)
    struct LRUCacheEntry {
        uint32_t block_index;
        uint32_t file_block_idx;
        std::vector<char> data;
        size_t access_count;
        std::chrono::steady_clock::time_point last_access;
    };
    std::unordered_map<uint32_t, LRUCacheEntry> lru_cache_;
    std::list<uint32_t> lru_list_; // For LRU eviction (most recent at front)
    mutable std::mutex lru_mutex_;
};

}

#endif
