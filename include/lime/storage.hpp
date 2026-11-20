#ifndef LIME_STORAGE_HPP
#define LIME_STORAGE_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <fstream>
#include <mutex>
#include <list>
#include <unordered_map>
#include <chrono>

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

class BlockStorage {
public:
    static constexpr uint32_t SECTOR_SIZE = 512;
    virtual ~BlockStorage() = default;
    virtual bool read_sectors(uint64_t lba, uint32_t sector_count, void* buffer) = 0;
    virtual bool write_sectors(uint64_t lba, uint32_t sector_count, const void* buffer) = 0;
    virtual uint64_t capacity_bytes() const = 0;
    virtual bool is_open() const = 0;
};

class SparseDisk : public BlockStorage {
public:
    static constexpr uint32_t SECTOR_SIZE = BlockStorage::SECTOR_SIZE;

    SparseDisk() = default;
    ~SparseDisk();

    // One contiguous portion of the virtual disk mapped to the backing file.
    // `allocated == false` means the block is a sparse hole (file_offset invalid).
    struct DiskRange {
        uint64_t virtual_offset;
        uint64_t file_offset;
        size_t length;
        bool allocated;
    };

    static bool create(const std::string& path, uint64_t capacity_bytes, uint32_t block_size = 65536);
    bool open(const std::string& path);
    void close();

    bool read_sectors(uint64_t lba, uint32_t sector_count, void* buffer) override;
    bool write_sectors(uint64_t lba, uint32_t sector_count, const void* buffer) override;

    // Async-I/O support: map a byte range to backing-file segments and (for
    // writes) ensure every block in the range is allocated on the host file.
    bool preallocate_range(uint64_t byte_offset, size_t length);
    bool map_range(uint64_t byte_offset, size_t length, std::vector<DiskRange>& out) const;

    const std::string& path() const { return file_path_; }

    uint64_t capacity_bytes() const override;
    uint64_t host_file_size() const;
    uint32_t block_size() const;
    uint32_t allocated_blocks() const;
    bool is_open() const override;

private:
    uint64_t get_block_file_offset(uint32_t block_index) const;
    uint32_t allocate_block(uint32_t block_index);

    std::string file_path_;
    mutable std::fstream file_;
    LimeSparseDiskHeader header_;
    std::vector<uint32_t> block_table_;
    mutable std::recursive_mutex mutex_;
    bool is_open_{false};

    static constexpr size_t LRU_CACHE_SIZE = 1024; 
    struct LRUCacheEntry {
        uint32_t block_index;
        uint32_t file_block_idx;
        std::vector<char> data;
        size_t access_count;
        std::chrono::steady_clock::time_point last_access;
    };
    std::unordered_map<uint32_t, LRUCacheEntry> lru_cache_;
    std::list<uint32_t> lru_list_; 
    mutable std::mutex lru_mutex_;
};

class RawDisk : public BlockStorage {
public:
    static constexpr uint32_t SECTOR_SIZE = BlockStorage::SECTOR_SIZE;

    RawDisk() = default;
    ~RawDisk() override;

    bool open(const std::string& path, bool read_only = false);
    void close();
    static bool create(const std::string& path, uint64_t capacity_bytes);

    bool read_sectors(uint64_t lba, uint32_t sector_count, void* buffer) override;
    bool write_sectors(uint64_t lba, uint32_t sector_count, const void* buffer) override;
    uint64_t capacity_bytes() const override;
    bool is_open() const override;

private:
    std::string path_;
    mutable std::fstream file_;
    uint64_t capacity_{0};
    bool read_only_{false};
    bool is_open_{false};
    mutable std::recursive_mutex mutex_;
};

#pragma pack(push, 1)
struct QCow2Header {
    uint32_t magic{0};
    uint32_t version{0};
    uint64_t backing_file_offset{0};
    uint32_t backing_file_size{0};
    uint32_t cluster_bits{0};
    uint64_t size{0};
    uint32_t crypt_method{0};
    uint32_t l1_size{0};
    uint64_t l1_table_offset{0};
    uint64_t refcount_table_offset{0};
    uint32_t refcount_table_clusters{0};
    uint32_t nb_snapshots{0};
    uint64_t snapshots_offset{0};
};
#pragma pack(pop)

class QCOW2Disk : public BlockStorage {
public:
    static constexpr uint32_t SECTOR_SIZE = BlockStorage::SECTOR_SIZE;
    static constexpr uint32_t QCOW2_MAGIC = 0x514649fb; // 'Q','F','I',0xfb

    QCOW2Disk() = default;
    ~QCOW2Disk() override;

    static bool create(const std::string& path, uint64_t capacity_bytes, uint32_t cluster_bits = 16);
    bool open(const std::string& path);
    void close();

    bool read_sectors(uint64_t lba, uint32_t sector_count, void* buffer) override;
    bool write_sectors(uint64_t lba, uint32_t sector_count, const void* buffer) override;
    uint64_t capacity_bytes() const override;
    bool is_open() const override;

    uint32_t cluster_size() const { return 1U << cluster_bits_; }
    uint32_t version() const { return version_; }

private:
    uint64_t allocate_cluster();

    std::string path_;
    mutable std::fstream file_;
    bool is_open_{false};
    uint32_t version_{3};
    uint32_t cluster_bits_{16};
    uint64_t capacity_bytes_{0};
    uint32_t l1_size_{0};
    uint64_t l1_table_offset_{0};
    std::vector<uint64_t> l1_table_;
    mutable std::recursive_mutex mutex_;
};

}

#endif
