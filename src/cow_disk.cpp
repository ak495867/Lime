#include "lime/cow_disk.hpp"
#include <algorithm>
#include <cstring>
#include <filesystem>

namespace lime {

bool CoWSparseDisk::create_overlay(const std::string& base_path, const std::string& delta_path) {
    std::error_code ec;
    if (std::filesystem::exists(delta_path, ec) || ec) return false;
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
    if (!buffer) return false;
    const auto* input = static_cast<const uint8_t*>(buffer);
    const uint32_t sectors_per_block = delta_disk_->block_size() / SparseDisk::SECTOR_SIZE;
    if (sectors_per_block == 0 || delta_disk_->block_size() % SparseDisk::SECTOR_SIZE != 0) return false;
    uint64_t remaining = sector_count;
    uint64_t current_lba = lba;
    while (remaining) {
        const uint32_t block_sector = static_cast<uint32_t>(current_lba % sectors_per_block);
        const uint32_t chunk_sectors = static_cast<uint32_t>(std::min<uint64_t>(remaining, sectors_per_block - block_sector));
        const uint64_t block_offset = current_lba * SparseDisk::SECTOR_SIZE -
                                      static_cast<uint64_t>(block_sector) * SparseDisk::SECTOR_SIZE;
        std::vector<SparseDisk::DiskRange> ranges;
        if (!delta_disk_->map_range(block_offset, 1, ranges) || ranges.empty()) return false;
        if (!ranges.front().allocated) {
            const uint64_t block_start_lba = block_offset / SparseDisk::SECTOR_SIZE;
            const uint32_t block_bytes = static_cast<uint32_t>(std::min<uint64_t>(
                delta_disk_->block_size(), capacity_bytes() - block_offset));
            const uint32_t block_sectors = block_bytes / SparseDisk::SECTOR_SIZE;
            std::vector<uint8_t> copy_up(static_cast<size_t>(block_sectors) * SparseDisk::SECTOR_SIZE);
            if (!base_disk_->read_sectors(block_start_lba, block_sectors, copy_up.data()) ||
                !delta_disk_->write_sectors(block_start_lba, block_sectors, copy_up.data())) return false;
        }
        const size_t chunk_bytes = static_cast<size_t>(chunk_sectors) * SparseDisk::SECTOR_SIZE;
        if (!delta_disk_->write_sectors(current_lba, chunk_sectors, input)) return false;
        current_lba += chunk_sectors;
        remaining -= chunk_sectors;
        input += chunk_bytes;
    }
    return true;
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
