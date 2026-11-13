#include "lime/cow_disk.hpp"
#include <algorithm>
#include <cstring>

namespace lime {

bool CoWSparseDisk::create_overlay(const std::string& base_path, const std::string& delta_path) {
    SparseDisk base;
    if (!base.open(base_path)) return false;
    uint64_t cap = base.capacity_bytes();
    uint32_t blk = base.block_size();
    base.close();

    return SparseDisk::create(delta_path, cap, blk);
}

bool CoWSparseDisk::open(const std::string& base_path, const std::string& delta_path) {
    close();
    base_disk_ = std::make_shared<SparseDisk>();
    delta_disk_ = std::make_shared<SparseDisk>();

    if (!base_disk_->open(base_path)) return false;
    if (!delta_disk_->open(delta_path)) return false;
    if (base_disk_->capacity_bytes() != delta_disk_->capacity_bytes() ||
        base_disk_->block_size() != delta_disk_->block_size()) {
        close();
        return false;
    }

    is_open_ = true;
    return true;
}

void CoWSparseDisk::close() {
    if (base_disk_) base_disk_->close();
    if (delta_disk_) delta_disk_->close();
    is_open_ = false;
}

bool CoWSparseDisk::read_sectors(uint64_t lba, uint32_t sector_count, void* buffer) {
    if (!is_open_) return false;
    if (!buffer || lba > UINT64_MAX / SparseDisk::SECTOR_SIZE) return false;
    const uint64_t offset = lba * SparseDisk::SECTOR_SIZE;
    const uint64_t bytes = static_cast<uint64_t>(sector_count) * SparseDisk::SECTOR_SIZE;
    if (offset > capacity_bytes() || bytes > capacity_bytes() - offset) return false;

    uint8_t* out = static_cast<uint8_t*>(buffer);
    uint64_t pos = offset;
    uint64_t remaining = bytes;
    while (remaining) {
        const uint32_t block_size = delta_disk_->block_size();
        const size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, block_size - (pos % block_size)));
        std::vector<SparseDisk::DiskRange> ranges;
        if (!delta_disk_->map_range(pos, chunk, ranges) || ranges.empty()) return false;
        if (ranges.front().allocated) {
            if (!delta_disk_->read_sectors(pos / SparseDisk::SECTOR_SIZE,
                    static_cast<uint32_t>(chunk / SparseDisk::SECTOR_SIZE), out)) return false;
        } else if (!base_disk_->read_sectors(pos / SparseDisk::SECTOR_SIZE,
                       static_cast<uint32_t>(chunk / SparseDisk::SECTOR_SIZE), out)) {
            return false;
        }
        pos += chunk;
        out += chunk;
        remaining -= chunk;
    }
    return true;
}

bool CoWSparseDisk::write_sectors(uint64_t lba, uint32_t sector_count, const void* buffer) {
    if (!is_open_) return false;
    if (!buffer || lba > UINT64_MAX / SparseDisk::SECTOR_SIZE) return false;
    const uint64_t offset = lba * SparseDisk::SECTOR_SIZE;
    const uint64_t bytes = static_cast<uint64_t>(sector_count) * SparseDisk::SECTOR_SIZE;
    if (offset > capacity_bytes() || bytes > capacity_bytes() - offset) return false;
    return delta_disk_->write_sectors(lba, sector_count, buffer);
}

uint64_t CoWSparseDisk::capacity_bytes() const {
    return base_disk_ ? base_disk_->capacity_bytes() : 0;
}

uint64_t CoWSparseDisk::delta_host_file_size() const {
    return delta_disk_ ? delta_disk_->host_file_size() : 0;
}

uint32_t CoWSparseDisk::delta_allocated_blocks() const {
    return delta_disk_ ? delta_disk_->allocated_blocks() : 0;
}

bool CoWSparseDisk::is_open() const {
    return is_open_;
}

}
