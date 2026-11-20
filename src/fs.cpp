#include "lime/fs.hpp"
#include <iostream>
#include <cstring>
#include <chrono>

namespace lime {

VirtIOFSDevice::VirtIOFSDevice(std::shared_ptr<MemoryManager> mem, uint64_t base_addr,
                               const std::string& host_path, const std::string& tag)
    : mem_(mem), base_addr_(base_addr), host_path_(host_path), tag_(tag) {
    if (tag_.size() > 36) tag_ = tag_.substr(0, 36);
    mount_host_dir(host_path_);
}

bool VirtIOFSDevice::mount_host_dir(const std::string& host_path) {
    std::lock_guard<std::mutex> lock(mutex_);
    host_path_ = host_path.empty() ? "." : host_path;
    std::error_code ec;
    std::filesystem::path abs_p = std::filesystem::absolute(host_path_, ec);
    node_to_path_[1] = abs_p;
    path_to_node_[abs_p.string()] = 1;
    mounted_ = true;
    return true;
}

uint32_t VirtIOFSDevice::read(uint64_t offset, size_t) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (offset) {
    case 0x00: return 0x74726976;  // Magic 'virt'
    case 0x04: return 2;           // Version 2 (modern MMIO)
    case 0x08: return 26;          // Device ID 26: VirtIO-FS
    case 0x0C: return 0x554D4551;  // Vendor ID
    case 0x10: return 1;           // Features
    case 0x14: return 128;         // Queue Num Max
    case 0x34: return queue_num_;  // Queue Num
    case 0x44: return vq_pfn_[queue_sel_ < 2 ? queue_sel_ : 0];
    case 0x60: return interrupt_status_;
    case 0x70: return status_;
    default:
        if (offset >= 0x100 && offset < 0x124) { // Tag buffer (36 bytes)
            size_t tag_offset = offset - 0x100;
            if (tag_offset < tag_.size()) {
                uint32_t val = 0;
                for (size_t i = 0; i < 4 && (tag_offset + i) < tag_.size(); ++i) {
                    val |= (static_cast<uint8_t>(tag_[tag_offset + i]) << (i * 8));
                }
                return val;
            }
            return 0;
        } else if (offset == 0x124) {
            return 1; // num_request_queues
        }
        return 0;
    }
}

void VirtIOFSDevice::write(uint64_t offset, uint32_t value, size_t) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (offset) {
    case 0x14:
        queue_sel_ = value;
        break;
    case 0x38:
        queue_align_ = value;
        break;
    case 0x3C:
        if (queue_sel_ < 2) vq_pfn_[queue_sel_] = value;
        break;
    case 0x50: // Queue Notify
        process_queue(value);
        break;
    case 0x64: // Interrupt ACK
        interrupt_status_ &= ~value;
        break;
    case 0x70: // Status
        status_ = value;
        break;
    default:
        break;
    }
}

fuse_attr VirtIOFSDevice::make_attr(uint64_t nodeid, const std::filesystem::path& p) {
    fuse_attr attr{};
    attr.ino = nodeid;
    attr.nlink = 1;
    attr.blksize = 4096;

    std::error_code ec;
    auto status = std::filesystem::status(p, ec);
    if (!ec) {
        if (std::filesystem::is_directory(status)) {
            attr.mode = 0040755; // S_IFDIR | 0755
            attr.size = 4096;
            attr.blocks = 8;
        } else {
            attr.mode = 0100644; // S_IFREG | 0644
            attr.size = std::filesystem::file_size(p, ec);
            attr.blocks = (attr.size + 511) / 512;
        }
    } else {
        attr.mode = 0100644;
        attr.size = 0;
        attr.blocks = 0;
    }

    auto now = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    attr.atime = now;
    attr.mtime = now;
    attr.ctime = now;
    return attr;
}

std::vector<uint8_t> VirtIOFSDevice::handle_fuse_request(const std::vector<uint8_t>& in_payload) {
    if (in_payload.size() < sizeof(fuse_in_header)) {
        std::vector<uint8_t> out(sizeof(fuse_out_header));
        auto* out_hdr = reinterpret_cast<fuse_out_header*>(out.data());
        out_hdr->len = sizeof(fuse_out_header);
        out_hdr->error = -22; // -EINVAL
        out_hdr->unique = 0;
        return out;
    }

    const auto* in_hdr = reinterpret_cast<const fuse_in_header*>(in_payload.data());
    uint64_t unique = in_hdr->unique;
    uint32_t opcode = in_hdr->opcode;
    uint64_t nodeid = in_hdr->nodeid;

    std::vector<uint8_t> response;

    switch (opcode) {
    case FUSE_INIT: {
        response.resize(sizeof(fuse_out_header) + sizeof(fuse_init_out), 0);
        auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
        out_hdr->len = static_cast<uint32_t>(response.size());
        out_hdr->error = 0;
        out_hdr->unique = unique;

        auto* init_out = reinterpret_cast<fuse_init_out*>(response.data() + sizeof(fuse_out_header));
        init_out->major = 7;
        init_out->minor = 31;
        init_out->max_readahead = 131072;
        init_out->flags = 0;
        init_out->max_write = 1048576;
        break;
    }
    case FUSE_LOOKUP: {
        const char* name_ptr = reinterpret_cast<const char*>(in_payload.data() + sizeof(fuse_in_header));
        std::string name(name_ptr);

        auto it = node_to_path_.find(nodeid);
        std::filesystem::path parent = (it != node_to_path_.end()) ? it->second : std::filesystem::path(host_path_);
        std::filesystem::path target = parent / name;

        std::error_code ec;
        if (std::filesystem::exists(target, ec)) {
            std::string abs_str = std::filesystem::absolute(target, ec).string();
            uint64_t target_node = 0;
            auto nit = path_to_node_.find(abs_str);
            if (nit != path_to_node_.end()) {
                target_node = nit->second;
            } else {
                target_node = next_nodeid_++;
                node_to_path_[target_node] = target;
                path_to_node_[abs_str] = target_node;
            }

            response.resize(sizeof(fuse_out_header) + sizeof(fuse_entry_out), 0);
            auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
            out_hdr->len = static_cast<uint32_t>(response.size());
            out_hdr->error = 0;
            out_hdr->unique = unique;

            auto* entry_out = reinterpret_cast<fuse_entry_out*>(response.data() + sizeof(fuse_out_header));
            entry_out->nodeid = target_node;
            entry_out->generation = 1;
            entry_out->entry_valid = 1;
            entry_out->attr_valid = 1;
            entry_out->attr = make_attr(target_node, target);
        } else {
            response.resize(sizeof(fuse_out_header));
            auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
            out_hdr->len = sizeof(fuse_out_header);
            out_hdr->error = -2; // -ENOENT
            out_hdr->unique = unique;
        }
        break;
    }
    case FUSE_GETATTR: {
        auto it = node_to_path_.find(nodeid);
        if (it != node_to_path_.end() && std::filesystem::exists(it->second)) {
            response.resize(sizeof(fuse_out_header) + sizeof(fuse_attr_out), 0);
            auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
            out_hdr->len = static_cast<uint32_t>(response.size());
            out_hdr->error = 0;
            out_hdr->unique = unique;

            auto* attr_out = reinterpret_cast<fuse_attr_out*>(response.data() + sizeof(fuse_out_header));
            attr_out->attr_valid = 1;
            attr_out->attr = make_attr(nodeid, it->second);
        } else {
            response.resize(sizeof(fuse_out_header));
            auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
            out_hdr->len = sizeof(fuse_out_header);
            out_hdr->error = -2; // -ENOENT
            out_hdr->unique = unique;
        }
        break;
    }
    case FUSE_OPEN: {
        auto it = node_to_path_.find(nodeid);
        if (it != node_to_path_.end() && std::filesystem::exists(it->second)) {
            uint64_t fh = next_fh_++;
            auto handle = std::make_shared<OpenHandle>();
            handle->path = it->second;
            handle->is_dir = std::filesystem::is_directory(it->second);
            if (!handle->is_dir) {
                handle->stream.open(it->second, std::ios::in | std::ios::out | std::ios::binary);
                if (!handle->stream.is_open()) {
                    handle->stream.open(it->second, std::ios::in | std::ios::binary);
                }
            }
            open_handles_[fh] = handle;

            response.resize(sizeof(fuse_out_header) + sizeof(fuse_open_out), 0);
            auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
            out_hdr->len = static_cast<uint32_t>(response.size());
            out_hdr->error = 0;
            out_hdr->unique = unique;

            auto* open_out = reinterpret_cast<fuse_open_out*>(response.data() + sizeof(fuse_out_header));
            open_out->fh = fh;
            open_out->open_flags = 0;
        } else {
            response.resize(sizeof(fuse_out_header));
            auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
            out_hdr->len = sizeof(fuse_out_header);
            out_hdr->error = -2;
            out_hdr->unique = unique;
        }
        break;
    }
    case FUSE_READ: {
        if (in_payload.size() < sizeof(fuse_in_header) + sizeof(fuse_read_in)) {
            response.resize(sizeof(fuse_out_header));
            auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
            out_hdr->len = sizeof(fuse_out_header);
            out_hdr->error = -22;
            out_hdr->unique = unique;
            break;
        }
        const auto* read_in = reinterpret_cast<const fuse_read_in*>(in_payload.data() + sizeof(fuse_in_header));
        auto it = open_handles_.find(read_in->fh);
        if (it != open_handles_.end() && !it->second->is_dir) {
            std::ifstream file(it->second->path, std::ios::binary);
            std::vector<uint8_t> buf(read_in->size, 0);
            size_t bytes_read = 0;
            if (file.is_open()) {
                file.seekg(static_cast<std::streamoff>(read_in->offset), std::ios::beg);
                file.read(reinterpret_cast<char*>(buf.data()), read_in->size);
                bytes_read = static_cast<size_t>(file.gcount());
            }
            buf.resize(bytes_read);

            response.resize(sizeof(fuse_out_header) + bytes_read);
            auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
            out_hdr->len = static_cast<uint32_t>(response.size());
            out_hdr->error = 0;
            out_hdr->unique = unique;
            if (bytes_read > 0) {
                std::memcpy(response.data() + sizeof(fuse_out_header), buf.data(), bytes_read);
            }
        } else {
            response.resize(sizeof(fuse_out_header));
            auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
            out_hdr->len = sizeof(fuse_out_header);
            out_hdr->error = -9; // -EBADF
            out_hdr->unique = unique;
        }
        break;
    }
    case FUSE_WRITE: {
        if (in_payload.size() < sizeof(fuse_in_header) + sizeof(fuse_write_in)) {
            response.resize(sizeof(fuse_out_header));
            auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
            out_hdr->len = sizeof(fuse_out_header);
            out_hdr->error = -22;
            out_hdr->unique = unique;
            break;
        }
        const auto* write_in = reinterpret_cast<const fuse_write_in*>(in_payload.data() + sizeof(fuse_in_header));
        const uint8_t* write_data = in_payload.data() + sizeof(fuse_in_header) + sizeof(fuse_write_in);
        uint32_t to_write = std::min(write_in->size, static_cast<uint32_t>(in_payload.size() - sizeof(fuse_in_header) - sizeof(fuse_write_in)));

        auto it = open_handles_.find(write_in->fh);
        if (it != open_handles_.end() && !it->second->is_dir) {
            std::fstream file(it->second->path, std::ios::in | std::ios::out | std::ios::binary);
            if (!file.is_open()) {
                file.open(it->second->path, std::ios::out | std::ios::binary);
            }
            if (file.is_open()) {
                file.seekp(static_cast<std::streamoff>(write_in->offset), std::ios::beg);
                file.write(reinterpret_cast<const char*>(write_data), to_write);
                file.flush();
            }

            response.resize(sizeof(fuse_out_header) + sizeof(fuse_write_out), 0);
            auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
            out_hdr->len = static_cast<uint32_t>(response.size());
            out_hdr->error = 0;
            out_hdr->unique = unique;

            auto* write_out = reinterpret_cast<fuse_write_out*>(response.data() + sizeof(fuse_out_header));
            write_out->size = to_write;
        } else {
            response.resize(sizeof(fuse_out_header));
            auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
            out_hdr->len = sizeof(fuse_out_header);
            out_hdr->error = -9;
            out_hdr->unique = unique;
        }
        break;
    }
    case FUSE_READDIR: {
        auto it = node_to_path_.find(nodeid);
        if (it != node_to_path_.end() && std::filesystem::is_directory(it->second)) {
            std::vector<uint8_t> dirents;
            uint64_t entry_idx = 1;
            std::error_code ec;
            for (const auto& entry : std::filesystem::directory_iterator(it->second, ec)) {
                std::string fname = entry.path().filename().string();
                size_t dsize = sizeof(fuse_dirent) + fname.size();
                size_t padded = (dsize + 7) & ~7ULL;

                std::vector<uint8_t> dentry(padded, 0);
                auto* d = reinterpret_cast<fuse_dirent*>(dentry.data());
                d->ino = next_nodeid_++;
                d->off = entry_idx++;
                d->namelen = static_cast<uint32_t>(fname.size());
                d->type = entry.is_directory() ? 4 : 8;
                std::memcpy(dentry.data() + sizeof(fuse_dirent), fname.data(), fname.size());

                dirents.insert(dirents.end(), dentry.begin(), dentry.end());
            }

            response.resize(sizeof(fuse_out_header) + dirents.size());
            auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
            out_hdr->len = static_cast<uint32_t>(response.size());
            out_hdr->error = 0;
            out_hdr->unique = unique;
            if (!dirents.empty()) {
                std::memcpy(response.data() + sizeof(fuse_out_header), dirents.data(), dirents.size());
            }
        } else {
            response.resize(sizeof(fuse_out_header));
            auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
            out_hdr->len = sizeof(fuse_out_header);
            out_hdr->error = -2;
            out_hdr->unique = unique;
        }
        break;
    }
    case FUSE_RELEASE: {
        const auto* open_in = reinterpret_cast<const fuse_open_in*>(in_payload.data() + sizeof(fuse_in_header));
        (void)open_in;
        open_handles_.erase(nodeid);
        response.resize(sizeof(fuse_out_header));
        auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
        out_hdr->len = sizeof(fuse_out_header);
        out_hdr->error = 0;
        out_hdr->unique = unique;
        break;
    }
    default: {
        response.resize(sizeof(fuse_out_header));
        auto* out_hdr = reinterpret_cast<fuse_out_header*>(response.data());
        out_hdr->len = sizeof(fuse_out_header);
        out_hdr->error = -38; // -ENOSYS
        out_hdr->unique = unique;
        break;
    }
    }

    return response;
}

void VirtIOFSDevice::process_queue(uint32_t q_idx) {
    if (q_idx >= 2 || !mem_) return;
    uint32_t pfn = vq_pfn_[q_idx];
    if (pfn == 0) return;

    const uint64_t vq_base = static_cast<uint64_t>(pfn) * queue_align_;
    const uint64_t desc_table_addr = vq_base;
    const uint64_t avail_addr = vq_base + static_cast<uint64_t>(queue_num_) * 16;
    const uint64_t used_addr = (avail_addr + 4 + queue_num_ * 2 + queue_align_ - 1) & ~(static_cast<uint64_t>(queue_align_) - 1);

    uint16_t avail_idx = mem_->read16(avail_addr + 2);

    while (last_avail_idx_[q_idx] != avail_idx) {
        uint16_t ring_entry = last_avail_idx_[q_idx] % queue_num_;
        uint16_t head_desc = mem_->read16(avail_addr + 4 + ring_entry * 2);
        last_avail_idx_[q_idx]++;

        uint16_t cur_desc = head_desc;
        std::vector<uint8_t> in_buffer;
        std::vector<std::pair<uint64_t, uint32_t>> out_buffers;
        int safety_limit = queue_num_ * 2;

        while (safety_limit-- > 0) {
            uint64_t d_addr = desc_table_addr + cur_desc * 16;
            uint64_t buf_addr = mem_->read64(d_addr);
            uint32_t buf_len = mem_->read32(d_addr + 8);
            uint16_t flags = mem_->read16(d_addr + 12);
            uint16_t next = mem_->read16(d_addr + 14);

            if (!(flags & 0x02)) { // Device-readable (guest input)
                size_t old_sz = in_buffer.size();
                in_buffer.resize(old_sz + buf_len);
                mem_->read_bytes(buf_addr, in_buffer.data() + old_sz, buf_len);
            } else { // Device-writable (guest output)
                out_buffers.emplace_back(buf_addr, buf_len);
            }

            if (!(flags & 0x01)) break;
            cur_desc = next;
        }

        std::vector<uint8_t> reply = handle_fuse_request(in_buffer);
        size_t reply_offset = 0;
        uint32_t total_written = 0;

        for (const auto& [out_addr, out_len] : out_buffers) {
            if (reply_offset >= reply.size()) break;
            uint32_t chunk = std::min(out_len, static_cast<uint32_t>(reply.size() - reply_offset));
            mem_->write_bytes(out_addr, reply.data() + reply_offset, chunk);
            reply_offset += chunk;
            total_written += chunk;
        }

        uint16_t used_idx = mem_->read16(used_addr + 2);
        uint16_t used_ring_entry = used_idx % queue_num_;
        uint64_t elem_addr = used_addr + 4 + used_ring_entry * 8;
        mem_->write32(elem_addr, head_desc);
        mem_->write32(elem_addr + 4, total_written);
        mem_->write16(used_addr + 2, used_idx + 1);

        interrupt_status_ |= 1;
    }
}

std::mutex LBFSBackend::mount_mutex_;

bool LBFSBackend::mount(const std::string&, const std::string&) {
    std::lock_guard<std::mutex> lock(mount_mutex_);
    return true;
}

bool LBFSBackend::umount(const std::string&) {
    std::lock_guard<std::mutex> lock(mount_mutex_);
    return true;
}

} // namespace lime
