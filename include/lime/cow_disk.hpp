#ifndef LIME_COW_DISK_HPP
#define LIME_COW_DISK_HPP

#include <memory>
#include <string>
#include "lime/storage.hpp"

namespace lime {

class CoWSparseDisk {
public:
    CoWSparseDisk() = default;
    ~CoWSparseDisk() = default;

    static bool create_overlay(const std::string& base_path, const std::string& delta_path);

    bool open(const std::string& base_path, const std::string& delta_path);
    void close();

    bool read_sectors(uint64_t lba, uint32_t sector_count, void* buffer);
    bool write_sectors(uint64_t lba, uint32_t sector_count, const void* buffer);

    uint64_t capacity_bytes() const;
    uint64_t delta_host_file_size() const;
    uint32_t delta_allocated_blocks() const;
    bool is_open() const;

private:
    std::shared_ptr<SparseDisk> base_disk_;
    std::shared_ptr<SparseDisk> delta_disk_;
    bool is_open_{false};
};

}

#endif
