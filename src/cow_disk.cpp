#include "lime/cow_disk.hpp"
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
    uint8_t* out = static_cast<uint8_t*>(buffer);

    for (uint32_t s = 0; s < sector_count; ++s) {
        uint64_t curr_lba = lba + s;
        uint64_t byte_pos = curr_lba * SparseDisk::SECTOR_SIZE;
        uint32_t blk_idx = static_cast<uint32_t>(byte_pos / delta_disk_->block_size());

        std::vector<uint8_t> tmp(SparseDisk::SECTOR_SIZE, 0);

        if (delta_disk_->read_sectors(curr_lba, 1, tmp.data())) {
            bool has_data = false;
            for (uint8_t b : tmp) {
                if (b != 0) { has_data = true; break; }
            }
            if (has_data || delta_disk_->allocated_blocks() > 0) {
                std::memcpy(out + (s * SparseDisk::SECTOR_SIZE), tmp.data(), SparseDisk::SECTOR_SIZE);
                continue;
            }
        }
        base_disk_->read_sectors(curr_lba, 1, out + (s * SparseDisk::SECTOR_SIZE));
    }

    return true;
}

bool CoWSparseDisk::write_sectors(uint64_t lba, uint32_t sector_count, const void* buffer) {
    if (!is_open_) return false;
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
