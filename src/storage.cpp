#include "lime/storage.hpp"
#include <cstring>
#include <algorithm>
#include <chrono>
#include <list>
#include <unordered_map>

namespace lime {

SparseDisk::~SparseDisk() {

    {
        std::lock_guard<std::mutex> lock(lru_mutex_);
        lru_cache_.clear();
        lru_list_.clear();
    }
    close();
}

bool SparseDisk::create(const std::string& path, uint64_t capacity_bytes, uint32_t block_size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    LimeSparseDiskHeader hdr;
    hdr.magic[0] = 'L';
    hdr.magic[1] = 'I';
    hdr.magic[2] = 'M';
    hdr.magic[3] = 'E';
    hdr.version = 1;
    hdr.virtual_capacity_bytes = capacity_bytes;
    hdr.block_size = block_size;
    hdr.total_blocks = static_cast<uint32_t>((capacity_bytes + block_size - 1) / block_size);
    hdr.allocated_blocks = 0;

    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    std::vector<uint32_t> table(hdr.total_blocks, 0xFFFFFFFF);
    out.write(reinterpret_cast<const char*>(table.data()), table.size() * sizeof(uint32_t));
    out.close();

    return true;
}

bool SparseDisk::open(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    close();

    file_path_ = path;
    file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) return false;

    file_.read(reinterpret_cast<char*>(&header_), sizeof(header_));
    if (file_.gcount() != sizeof(header_)) {
        close();
        return false;
    }

    if (header_.magic[0] != 'L' || header_.magic[1] != 'I' ||
        header_.magic[2] != 'M' || header_.magic[3] != 'E') {
        close();
        return false;
    }

    block_table_.resize(header_.total_blocks);
    file_.read(reinterpret_cast<char*>(block_table_.data()), header_.total_blocks * sizeof(uint32_t));
    if (file_.gcount() != static_cast<std::streamsize>(header_.total_blocks * sizeof(uint32_t))) {
        close();
        return false;
    }

    is_open_ = true;
    return true;
}

void SparseDisk::close() {
    if (file_.is_open()) {
        file_.close();
    }
    is_open_ = false;
    block_table_.clear();
}

uint32_t SparseDisk::allocate_block(uint32_t block_index) {
    file_.seekp(0, std::ios::end);
    uint64_t offset = file_.tellp();

    std::vector<char> empty_block(header_.block_size, 0);
    file_.write(empty_block.data(), empty_block.size());
    file_.flush();

    uint32_t file_block_idx = header_.allocated_blocks;
    header_.allocated_blocks++;

    file_.seekp(0, std::ios::beg);
    file_.write(reinterpret_cast<const char*>(&header_), sizeof(header_));

    uint64_t table_entry_offset = sizeof(header_) + (block_index * sizeof(uint32_t));
    file_.seekp(table_entry_offset, std::ios::beg);
    file_.write(reinterpret_cast<const char*>(&file_block_idx), sizeof(file_block_idx));
    file_.flush();

    block_table_[block_index] = file_block_idx;
    return file_block_idx;
}

uint64_t SparseDisk::get_block_file_offset(uint32_t block_index) const {
    if (block_index >= header_.total_blocks) return 0;
    uint32_t file_block_idx = block_table_[block_index];
    if (file_block_idx == 0xFFFFFFFF) return 0;

    uint64_t header_table_size = sizeof(LimeSparseDiskHeader) + (header_.total_blocks * sizeof(uint32_t));
    return header_table_size + (static_cast<uint64_t>(file_block_idx) * header_.block_size);
}

bool SparseDisk::preallocate_range(uint64_t byte_offset, size_t length) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) return false;
    if (length == 0) return true;
    if (byte_offset + length > header_.virtual_capacity_bytes) return false;

    uint64_t end = byte_offset + length;
    uint64_t pos = byte_offset;
    while (pos < end) {
        uint32_t blk_idx = static_cast<uint32_t>(pos / header_.block_size);
        if (blk_idx >= header_.total_blocks) return false;
        if (block_table_[blk_idx] == 0xFFFFFFFF) {
            allocate_block(blk_idx);
        }
        pos += header_.block_size - (pos % header_.block_size);
    }
    return true;
}

bool SparseDisk::map_range(uint64_t byte_offset, size_t length, std::vector<DiskRange>& out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) return false;
    if (length == 0) return true;
    if (byte_offset + length > header_.virtual_capacity_bytes) return false;

    out.clear();
    uint64_t remaining = length;
    uint64_t pos = byte_offset;
    while (remaining > 0) {
        uint32_t blk_idx = static_cast<uint32_t>(pos / header_.block_size);
        uint32_t offset_in_blk = static_cast<uint32_t>(pos % header_.block_size);
        size_t chunk = static_cast<size_t>(std::min<uint64_t>(remaining, header_.block_size - offset_in_blk));

        DiskRange r;
        r.virtual_offset = pos;
        r.length = chunk;
        r.allocated = (block_table_[blk_idx] != 0xFFFFFFFF);
        r.file_offset = r.allocated ? (get_block_file_offset(blk_idx) + offset_in_blk) : 0;
        out.push_back(r);

        pos += chunk;
        remaining -= chunk;
    }
    return true;
}

bool SparseDisk::read_sectors(uint64_t lba, uint32_t sector_count, void* buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) return false;

    uint64_t byte_offset = lba * SECTOR_SIZE;
    uint64_t total_bytes = static_cast<uint64_t>(sector_count) * SECTOR_SIZE;

    if (byte_offset + total_bytes > header_.virtual_capacity_bytes) {
        return false;
    }

    uint8_t* out = static_cast<uint8_t*>(buffer);
    uint64_t remaining = total_bytes;
    uint64_t curr_pos = byte_offset;

    while (remaining > 0) {
        uint32_t blk_idx = static_cast<uint32_t>(curr_pos / header_.block_size);
        uint32_t offset_in_blk = static_cast<uint32_t>(curr_pos % header_.block_size);
        size_t chunk = std::min<size_t>(remaining, header_.block_size - offset_in_blk);

        uint64_t file_offset = get_block_file_offset(blk_idx);
        if (file_offset == 0) {
            std::memset(out, 0, chunk);
        } else {
            file_.seekg(file_offset + offset_in_blk, std::ios::beg);
            file_.read(reinterpret_cast<char*>(out), chunk);
        }

        out += chunk;
        curr_pos += chunk;
        remaining -= chunk;
    }

    return true;
}

bool SparseDisk::write_sectors(uint64_t lba, uint32_t sector_count, const void* buffer) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_open_) return false;

    uint64_t byte_offset = lba * SECTOR_SIZE;
    uint64_t total_bytes = static_cast<uint64_t>(sector_count) * SECTOR_SIZE;

    if (byte_offset + total_bytes > header_.virtual_capacity_bytes) {
        return false;
    }

    const uint8_t* in = static_cast<const uint8_t*>(buffer);
    uint64_t remaining = total_bytes;
    uint64_t curr_pos = byte_offset;

    while (remaining > 0) {
        uint32_t blk_idx = static_cast<uint32_t>(curr_pos / header_.block_size);
        uint32_t offset_in_blk = static_cast<uint32_t>(curr_pos % header_.block_size);
        size_t chunk = std::min<size_t>(remaining, header_.block_size - offset_in_blk);

        uint64_t file_offset = get_block_file_offset(blk_idx);
        if (file_offset == 0) {
            allocate_block(blk_idx);
            file_offset = get_block_file_offset(blk_idx);
        }

        file_.seekp(file_offset + offset_in_blk, std::ios::beg);
        file_.write(reinterpret_cast<const char*>(in), chunk);
        file_.flush();

        in += chunk;
        curr_pos += chunk;
        remaining -= chunk;
    }

    return true;
}

uint64_t SparseDisk::capacity_bytes() const {
    return header_.virtual_capacity_bytes;
}

uint64_t SparseDisk::host_file_size() const {
    if (!is_open_) return 0;
    return sizeof(LimeSparseDiskHeader) + (header_.total_blocks * sizeof(uint32_t)) +
           (static_cast<uint64_t>(header_.allocated_blocks) * header_.block_size);
}

uint32_t SparseDisk::block_size() const {
    return header_.block_size;
}

uint32_t SparseDisk::allocated_blocks() const {
    return header_.allocated_blocks;
}

bool SparseDisk::is_open() const {
    return is_open_;
}

}
