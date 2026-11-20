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
    if (capacity_bytes == 0 || block_size == 0 ||
        capacity_bytes % SECTOR_SIZE != 0 || block_size % SECTOR_SIZE != 0) return false;
    const uint64_t total_blocks = capacity_bytes / block_size + (capacity_bytes % block_size != 0);
    if (total_blocks > UINT32_MAX) return false;
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
    hdr.total_blocks = static_cast<uint32_t>(total_blocks);
    hdr.allocated_blocks = 0;

    out.write(reinterpret_cast<const char*>(&hdr), sizeof(hdr));

    std::vector<uint32_t> table(hdr.total_blocks, 0xFFFFFFFF);
    out.write(reinterpret_cast<const char*>(table.data()), table.size() * sizeof(uint32_t));
    out.close();

    return true;
}

bool SparseDisk::open(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
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

    if (header_.version != 1 || header_.block_size == 0 ||
        header_.block_size % SECTOR_SIZE != 0 ||
        header_.virtual_capacity_bytes % SECTOR_SIZE != 0 ||
        header_.total_blocks == 0 ||
        header_.allocated_blocks > header_.total_blocks ||
        (header_.virtual_capacity_bytes / header_.block_size +
         (header_.virtual_capacity_bytes % header_.block_size != 0)) != header_.total_blocks) {
        close();
        return false;
    }

    file_.seekg(0, std::ios::end);
    const std::streamoff physical_size = file_.tellg();
    const uint64_t table_end = sizeof(LimeSparseDiskHeader) +
                               static_cast<uint64_t>(header_.total_blocks) * sizeof(uint32_t);
    if (physical_size < 0 || static_cast<uint64_t>(physical_size) < table_end) {
        close();
        return false;
    }

    block_table_.resize(header_.total_blocks);
    file_.clear();
    file_.seekg(sizeof(LimeSparseDiskHeader), std::ios::beg);
    file_.read(reinterpret_cast<char*>(block_table_.data()), header_.total_blocks * sizeof(uint32_t));
    if (file_.gcount() != static_cast<std::streamsize>(header_.total_blocks * sizeof(uint32_t))) {
        close();
        return false;
    }

    std::vector<bool> seen(header_.allocated_blocks, false);
    uint32_t mapped_blocks = 0;
    for (uint32_t file_block : block_table_) {
        if (file_block == 0xFFFFFFFF) continue;
        if (file_block >= header_.allocated_blocks || seen[file_block]) {
            close();
            return false;
        }
        seen[file_block] = true;
        ++mapped_blocks;
    }
    const uint64_t data_bytes = static_cast<uint64_t>(header_.allocated_blocks) * header_.block_size;
    if (mapped_blocks != header_.allocated_blocks ||
        data_bytes > UINT64_MAX - table_end ||
        static_cast<uint64_t>(physical_size) < table_end + data_bytes) {
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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_open_) return false;
    if (length == 0) return true;
    if (byte_offset > header_.virtual_capacity_bytes || length > header_.virtual_capacity_bytes - byte_offset) return false;

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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_open_) return false;
    if (length == 0) return true;
    if (byte_offset > header_.virtual_capacity_bytes || length > header_.virtual_capacity_bytes - byte_offset) return false;

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
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_open_) return false;

    if (!buffer || lba > UINT64_MAX / SECTOR_SIZE) return false;
    uint64_t byte_offset = lba * SECTOR_SIZE;
    if (sector_count > (UINT64_MAX - byte_offset) / SECTOR_SIZE) return false;
    uint64_t total_bytes = static_cast<uint64_t>(sector_count) * SECTOR_SIZE;

    if (byte_offset > header_.virtual_capacity_bytes || total_bytes > header_.virtual_capacity_bytes - byte_offset) {
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
            if (file_.gcount() != static_cast<std::streamsize>(chunk)) return false;
            file_.clear();
        }

        out += chunk;
        curr_pos += chunk;
        remaining -= chunk;
    }

    return true;
}

bool SparseDisk::write_sectors(uint64_t lba, uint32_t sector_count, const void* buffer) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_open_) return false;

    if (!buffer || lba > UINT64_MAX / SECTOR_SIZE) return false;
    uint64_t byte_offset = lba * SECTOR_SIZE;
    if (sector_count > (UINT64_MAX - byte_offset) / SECTOR_SIZE) return false;
    uint64_t total_bytes = static_cast<uint64_t>(sector_count) * SECTOR_SIZE;

    if (byte_offset > header_.virtual_capacity_bytes || total_bytes > header_.virtual_capacity_bytes - byte_offset) {
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
        if (!file_) return false;
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

RawDisk::~RawDisk() {
    close();
}

bool RawDisk::open(const std::string& path, bool read_only) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    close();

    std::ios_base::openmode mode = std::ios::binary | std::ios::in;
    if (!read_only) mode |= std::ios::out;
    file_.open(path, mode);
    if (!file_.is_open() && !read_only) {
        file_.open(path, std::ios::binary | std::ios::in);
        read_only = true;
    }
    if (!file_.is_open()) return false;

    file_.seekg(0, std::ios::end);
    std::streampos size = file_.tellg();
    if (size < 0) {
        file_.close();
        return false;
    }
    capacity_ = static_cast<uint64_t>(size);
    path_ = path;
    read_only_ = read_only;
    is_open_ = true;
    return true;
}

void RawDisk::close() {
    if (file_.is_open()) {
        file_.flush();
        file_.close();
    }
    is_open_ = false;
    capacity_ = 0;
}

bool RawDisk::create(const std::string& path, uint64_t capacity_bytes) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f.is_open()) return false;
    if (capacity_bytes > 0) {
        f.seekp(capacity_bytes - 1);
        f.put('\0');
    }
    return f.good();
}

bool RawDisk::read_sectors(uint64_t lba, uint32_t sector_count, void* buffer) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_open_ || !buffer) return false;
    if (lba > UINT64_MAX / SECTOR_SIZE) return false;
    uint64_t offset = lba * SECTOR_SIZE;
    if (sector_count > (UINT64_MAX - offset) / SECTOR_SIZE) return false;
    uint64_t bytes = static_cast<uint64_t>(sector_count) * SECTOR_SIZE;
    if (offset > capacity_ || bytes > capacity_ - offset) return false;

    file_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.read(static_cast<char*>(buffer), static_cast<std::streamsize>(bytes));
    return file_.good() || file_.gcount() == static_cast<std::streamsize>(bytes);
}

bool RawDisk::write_sectors(uint64_t lba, uint32_t sector_count, const void* buffer) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_open_ || !buffer || read_only_) return false;
    if (lba > UINT64_MAX / SECTOR_SIZE) return false;
    uint64_t offset = lba * SECTOR_SIZE;
    if (sector_count > (UINT64_MAX - offset) / SECTOR_SIZE) return false;
    uint64_t bytes = static_cast<uint64_t>(sector_count) * SECTOR_SIZE;
    if (offset > capacity_ || bytes > capacity_ - offset) return false;

    file_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    file_.write(static_cast<const char*>(buffer), static_cast<std::streamsize>(bytes));
    file_.flush();
    return file_.good();
}

uint64_t RawDisk::capacity_bytes() const {
    return capacity_;
}

bool RawDisk::is_open() const {
    return is_open_;
}

namespace {

inline uint32_t qcow2_bswap32(uint32_t val) {
#if defined(_MSC_VER)
    return _byteswap_ulong(val);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap32(val);
#else
    return ((val & 0xFF000000u) >> 24) | ((val & 0x00FF0000u) >> 8) |
           ((val & 0x0000FF00u) << 8)  | ((val & 0x000000FFu) << 24);
#endif
}

inline uint64_t qcow2_bswap64(uint64_t val) {
#if defined(_MSC_VER)
    return _byteswap_uint64(val);
#elif defined(__GNUC__) || defined(__clang__)
    return __builtin_bswap64(val);
#else
    return ((val & 0xFF00000000000000ULL) >> 56) |
           ((val & 0x00FF000000000000ULL) >> 40) |
           ((val & 0x0000FF0000000000ULL) >> 24) |
           ((val & 0x000000FF00000000ULL) >> 8)  |
           ((val & 0x00000000FF000000ULL) << 8)  |
           ((val & 0x0000000000FF0000ULL) << 24) |
           ((val & 0x000000000000FF00ULL) << 40) |
           ((val & 0x00000000000000FFULL) << 56);
#endif
}

} // namespace

QCOW2Disk::~QCOW2Disk() {
    close();
}

void QCOW2Disk::close() {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (file_.is_open()) file_.close();
    is_open_ = false;
    l1_table_.clear();
}

bool QCOW2Disk::create(const std::string& path, uint64_t capacity_bytes, uint32_t cluster_bits) {
    if (cluster_bits < 9 || cluster_bits > 21) cluster_bits = 16;
    const uint64_t cluster_size = 1ULL << cluster_bits;
    const uint64_t l2_entries = cluster_size / sizeof(uint64_t);
    const uint64_t total_clusters = (capacity_bytes + cluster_size - 1) / cluster_size;
    const uint32_t l1_size = static_cast<uint32_t>((total_clusters + l2_entries - 1) / l2_entries);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) return false;

    QCow2Header hdr{};
    hdr.magic = qcow2_bswap32(QCOW2_MAGIC);
    hdr.version = qcow2_bswap32(3);
    hdr.cluster_bits = qcow2_bswap32(cluster_bits);
    hdr.size = qcow2_bswap64(capacity_bytes);
    hdr.crypt_method = 0;
    hdr.l1_size = qcow2_bswap32(l1_size);
    hdr.l1_table_offset = qcow2_bswap64(cluster_size);
    hdr.refcount_table_offset = qcow2_bswap64(cluster_size * 2);
    hdr.refcount_table_clusters = qcow2_bswap32(1);

    std::vector<char> cluster_buf(cluster_size, 0);
    std::memcpy(cluster_buf.data(), &hdr, sizeof(hdr));
    out.write(cluster_buf.data(), cluster_size);

    std::fill(cluster_buf.begin(), cluster_buf.end(), 0);
    out.write(cluster_buf.data(), cluster_size);
    out.write(cluster_buf.data(), cluster_size);
    out.close();
    return true;
}

bool QCOW2Disk::open(const std::string& path) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    close();

    path_ = path;
    file_.open(path, std::ios::in | std::ios::out | std::ios::binary);
    if (!file_.is_open()) return false;

    QCow2Header hdr{};
    file_.read(reinterpret_cast<char*>(&hdr), sizeof(hdr));
    if (file_.gcount() != sizeof(hdr)) {
        close();
        return false;
    }

    uint32_t magic = qcow2_bswap32(hdr.magic);
    if (magic != QCOW2_MAGIC) {
        close();
        return false;
    }

    version_ = qcow2_bswap32(hdr.version);
    if (version_ != 2 && version_ != 3) {
        close();
        return false;
    }

    cluster_bits_ = qcow2_bswap32(hdr.cluster_bits);
    if (cluster_bits_ < 9 || cluster_bits_ > 21) {
        close();
        return false;
    }

    capacity_bytes_ = qcow2_bswap64(hdr.size);
    l1_size_ = qcow2_bswap32(hdr.l1_size);
    l1_table_offset_ = qcow2_bswap64(hdr.l1_table_offset);

    l1_table_.resize(l1_size_, 0);
    if (l1_size_ > 0) {
        file_.seekg(static_cast<std::streamoff>(l1_table_offset_), std::ios::beg);
        file_.read(reinterpret_cast<char*>(l1_table_.data()), l1_size_ * sizeof(uint64_t));
        if (file_.gcount() != static_cast<std::streamsize>(l1_size_ * sizeof(uint64_t))) {
            close();
            return false;
        }
        for (auto& entry : l1_table_) {
            entry = qcow2_bswap64(entry);
        }
    }

    is_open_ = true;
    return true;
}

uint64_t QCOW2Disk::allocate_cluster() {
    file_.clear();
    file_.seekp(0, std::ios::end);
    uint64_t offset = static_cast<uint64_t>(file_.tellp());
    const uint64_t csize = cluster_size();
    if (offset % csize != 0) {
        offset = ((offset + csize - 1) / csize) * csize;
        file_.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    }
    std::vector<char> zeros(csize, 0);
    file_.write(zeros.data(), csize);
    file_.flush();
    return offset;
}

bool QCOW2Disk::read_sectors(uint64_t lba, uint32_t sector_count, void* buffer) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_open_ || !buffer) return false;

    uint64_t offset = lba * SECTOR_SIZE;
    uint64_t total_bytes = static_cast<uint64_t>(sector_count) * SECTOR_SIZE;
    if (offset > capacity_bytes_ || total_bytes > capacity_bytes_ - offset) return false;

    const uint64_t csize = cluster_size();
    const uint64_t l2_entries = csize / sizeof(uint64_t);
    uint8_t* dst = static_cast<uint8_t*>(buffer);

    while (total_bytes > 0) {
        uint64_t cluster_idx = offset / csize;
        uint64_t cluster_off = offset % csize;
        uint64_t chunk = std::min(total_bytes, csize - cluster_off);

        uint64_t l1_idx = cluster_idx / l2_entries;
        uint64_t l2_idx = cluster_idx % l2_entries;

        if (l1_idx >= l1_size_ || l1_table_[l1_idx] == 0) {
            std::memset(dst, 0, chunk);
        } else {
            uint64_t l2_offset = l1_table_[l1_idx] & 0x00FFFFFFFFFFFF00ULL;
            file_.clear();
            file_.seekg(static_cast<std::streamoff>(l2_offset + l2_idx * sizeof(uint64_t)), std::ios::beg);
            uint64_t l2_entry = 0;
            file_.read(reinterpret_cast<char*>(&l2_entry), sizeof(uint64_t));
            l2_entry = qcow2_bswap64(l2_entry);

            uint64_t data_cluster = l2_entry & 0x00FFFFFFFFFFFF00ULL;
            if (data_cluster == 0) {
                std::memset(dst, 0, chunk);
            } else {
                file_.clear();
                file_.seekg(static_cast<std::streamoff>(data_cluster + cluster_off), std::ios::beg);
                file_.read(reinterpret_cast<char*>(dst), chunk);
            }
        }

        dst += chunk;
        offset += chunk;
        total_bytes -= chunk;
    }
    return true;
}

bool QCOW2Disk::write_sectors(uint64_t lba, uint32_t sector_count, const void* buffer) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (!is_open_ || !buffer) return false;

    uint64_t offset = lba * SECTOR_SIZE;
    uint64_t total_bytes = static_cast<uint64_t>(sector_count) * SECTOR_SIZE;
    if (offset > capacity_bytes_ || total_bytes > capacity_bytes_ - offset) return false;

    const uint64_t csize = cluster_size();
    const uint64_t l2_entries = csize / sizeof(uint64_t);
    const uint8_t* src = static_cast<const uint8_t*>(buffer);

    while (total_bytes > 0) {
        uint64_t cluster_idx = offset / csize;
        uint64_t cluster_off = offset % csize;
        uint64_t chunk = std::min(total_bytes, csize - cluster_off);

        uint64_t l1_idx = cluster_idx / l2_entries;
        uint64_t l2_idx = cluster_idx % l2_entries;

        if (l1_idx >= l1_size_) return false;

        if (l1_table_[l1_idx] == 0) {
            uint64_t new_l2 = allocate_cluster();
            l1_table_[l1_idx] = new_l2;
            file_.clear();
            file_.seekp(static_cast<std::streamoff>(l1_table_offset_ + l1_idx * sizeof(uint64_t)), std::ios::beg);
            uint64_t be_val = qcow2_bswap64(new_l2);
            file_.write(reinterpret_cast<const char*>(&be_val), sizeof(uint64_t));
        }

        uint64_t l2_offset = l1_table_[l1_idx] & 0x00FFFFFFFFFFFF00ULL;
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(l2_offset + l2_idx * sizeof(uint64_t)), std::ios::beg);
        uint64_t l2_entry = 0;
        file_.read(reinterpret_cast<char*>(&l2_entry), sizeof(uint64_t));
        l2_entry = qcow2_bswap64(l2_entry);

        uint64_t data_cluster = l2_entry & 0x00FFFFFFFFFFFF00ULL;
        if (data_cluster == 0) {
            data_cluster = allocate_cluster();
            file_.clear();
            file_.seekp(static_cast<std::streamoff>(l2_offset + l2_idx * sizeof(uint64_t)), std::ios::beg);
            uint64_t be_data = qcow2_bswap64(data_cluster);
            file_.write(reinterpret_cast<const char*>(&be_data), sizeof(uint64_t));
        }

        file_.clear();
        file_.seekp(static_cast<std::streamoff>(data_cluster + cluster_off), std::ios::beg);
        file_.write(reinterpret_cast<const char*>(src), chunk);

        src += chunk;
        offset += chunk;
        total_bytes -= chunk;
    }
    file_.flush();
    return true;
}

uint64_t QCOW2Disk::capacity_bytes() const {
    return capacity_bytes_;
}

bool QCOW2Disk::is_open() const {
    return is_open_;
}

}
