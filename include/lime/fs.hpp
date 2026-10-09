#ifndef LIME_FS_HPP
#define LIME_FS_HPP

#include <cstdint>
#include <string>
#include <memory>
#include <mutex>
#include "lime/devices.hpp"
#include "lime/memory.hpp"

namespace lime {

class VirtIOFileSystemDevice : public Device {
public:
    VirtIOFileSystemDevice(std::shared_ptr<MemoryManager> mem, uint64_t base_addr, const std::string& host_path);
    ~VirtIOFileSystemDevice() override = default;

    std::string name() const override { return "VirtIO-9p"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x1000; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

    bool mount_host_dir(const std::string& host_path);

private:
    std::shared_ptr<MemoryManager> mem_;
    uint64_t base_addr_;
    std::string host_mount_path_;
    bool mounted_{false};

    uint32_t handle_9p_version(size_t size);
    uint32_t handle_9p_attach(size_t size);
    uint32_t handle_9p_open(size_t size);
};

class LBFSBackend {
public:
    static bool mount(const std::string& host_dir, const std::string& guest_mount_point);
    static bool umount(const std::string& guest_mount_point);

private:
    static std::mutex mount_mutex_;
};

}

#endif
