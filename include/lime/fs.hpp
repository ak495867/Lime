#ifndef LIME_FS_HPP
#define LIME_FS_HPP

#include <cstdint>
#include <string>
#include <memory>
#include <mutex>
#include <vector>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include "lime/devices.hpp"
#include "lime/memory.hpp"

namespace lime {

#pragma pack(push, 1)
struct fuse_in_header {
    uint32_t len;
    uint32_t opcode;
    uint64_t unique;
    uint64_t nodeid;
    uint32_t uid;
    uint32_t gid;
    uint32_t pid;
    uint32_t padding;
};

struct fuse_out_header {
    uint32_t len;
    int32_t error;
    uint64_t unique;
};

struct fuse_init_in {
    uint32_t major;
    uint32_t minor;
    uint32_t max_readahead;
    uint32_t flags;
};

struct fuse_init_out {
    uint32_t major;
    uint32_t minor;
    uint32_t max_readahead;
    uint32_t flags;
    uint16_t max_background;
    uint16_t congestion_threshold;
    uint32_t max_write;
    uint32_t time_gran;
    uint32_t unused[9];
};

struct fuse_attr {
    uint64_t ino;
    uint64_t size;
    uint64_t blocks;
    uint64_t atime;
    uint64_t mtime;
    uint64_t ctime;
    uint32_t atimensec;
    uint32_t mtimensec;
    uint32_t ctimensec;
    uint32_t mode;
    uint32_t nlink;
    uint32_t uid;
    uint32_t gid;
    uint32_t rdev;
    uint32_t blksize;
    uint32_t padding;
};

struct fuse_attr_out {
    uint64_t attr_valid;
    uint32_t attr_valid_nsec;
    uint32_t dummy;
    fuse_attr attr;
};

struct fuse_entry_out {
    uint64_t nodeid;
    uint64_t generation;
    uint64_t entry_valid;
    uint64_t attr_valid;
    uint32_t entry_valid_nsec;
    uint32_t attr_valid_nsec;
    fuse_attr attr;
};

struct fuse_open_in {
    uint32_t flags;
    uint32_t unused;
};

struct fuse_open_out {
    uint64_t fh;
    uint32_t open_flags;
    uint32_t padding;
};

struct fuse_read_in {
    uint64_t fh;
    uint64_t offset;
    uint32_t size;
    uint32_t read_flags;
    uint64_t lock_owner;
    uint32_t flags;
    uint32_t padding;
};

struct fuse_write_in {
    uint64_t fh;
    uint64_t offset;
    uint32_t size;
    uint32_t write_flags;
    uint64_t lock_owner;
    uint32_t flags;
    uint32_t padding;
};

struct fuse_write_out {
    uint32_t size;
    uint32_t padding;
};

struct fuse_dirent {
    uint64_t ino;
    uint64_t off;
    uint32_t namelen;
    uint32_t type;
};
#pragma pack(pop)

enum FuseOpcode {
    FUSE_LOOKUP      = 1,
    FUSE_FORGET      = 2,
    FUSE_GETATTR     = 3,
    FUSE_SETATTR     = 4,
    FUSE_READLINK    = 5,
    FUSE_SYMLINK     = 6,
    FUSE_MKNOD       = 8,
    FUSE_MKDIR       = 9,
    FUSE_UNLINK      = 10,
    FUSE_RMDIR       = 11,
    FUSE_RENAME      = 12,
    FUSE_LINK        = 13,
    FUSE_OPEN        = 14,
    FUSE_READ        = 15,
    FUSE_WRITE       = 16,
    FUSE_STATFS      = 17,
    FUSE_RELEASE     = 18,
    FUSE_FSYNC       = 20,
    FUSE_INIT        = 26,
    FUSE_OPENDIR     = 27,
    FUSE_READDIR     = 28,
    FUSE_RELEASEDIR  = 29
};

class VirtIOFSDevice : public Device {
public:
    VirtIOFSDevice(std::shared_ptr<MemoryManager> mem, uint64_t base_addr = 0x10006000,
                   const std::string& host_path = ".", const std::string& tag = "limefs");
    ~VirtIOFSDevice() override = default;

    std::string name() const override { return "VirtIO-FS"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x1000; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

    bool mount_host_dir(const std::string& host_path);
    const std::string& host_path() const { return host_path_; }
    const std::string& tag() const { return tag_; }

    void process_queue(uint32_t queue_idx);

    // Process a direct in-memory FUSE request payload and return reply buffer
    std::vector<uint8_t> handle_fuse_request(const std::vector<uint8_t>& in_payload);

private:
    std::shared_ptr<MemoryManager> mem_;
    uint64_t base_addr_;
    std::string host_path_;
    std::string tag_;
    bool mounted_{false};
    uint32_t status_{0};
    uint32_t queue_sel_{0};
    uint32_t queue_num_{128};
    uint32_t queue_align_{4096};
    uint32_t vq_pfn_[2]{0, 0};
    uint16_t last_avail_idx_[2]{0, 0};
    uint32_t interrupt_status_{0};

    // Node ID and file handle tracking
    uint64_t next_nodeid_{2};
    std::unordered_map<uint64_t, std::filesystem::path> node_to_path_;
    std::unordered_map<std::string, uint64_t> path_to_node_;

    uint64_t next_fh_{1};
    struct OpenHandle {
        std::filesystem::path path;
        std::fstream stream;
        bool is_dir{false};
    };
    std::unordered_map<uint64_t, std::shared_ptr<OpenHandle>> open_handles_;
    mutable std::mutex mutex_;

    fuse_attr make_attr(uint64_t nodeid, const std::filesystem::path& p);
};

// Backwards compatibility alias
using VirtIOFileSystemDevice = VirtIOFSDevice;

class LBFSBackend {
public:
    static bool mount(const std::string& host_dir, const std::string& guest_mount_point);
    static bool umount(const std::string& guest_mount_point);

private:
    static std::mutex mount_mutex_;
};

}

#endif
