#include "lime/fs.hpp"
#include <iostream>
#include <cstring>

namespace lime {

VirtIOFileSystemDevice::VirtIOFileSystemDevice(std::shared_ptr<MemoryManager> mem, uint64_t base_addr, const std::string& host_path)
    : mem_(mem), base_addr_(base_addr), host_mount_path_(host_path), mounted_(false) {}

uint32_t VirtIOFileSystemDevice::read(uint64_t offset, size_t) {
    switch (offset) {
    case 0x00: return 0x74726976;  
    case 0x04: return 2;           
    case 0x08: return 1;           
    case 0x70: return static_cast<uint32_t>(mounted_ ? 1 : 0);  
    default: return 0;
    }
}

void VirtIOFileSystemDevice::write(uint64_t offset, uint32_t value, size_t) {
    switch (offset) {
    case 0x70:

        if (value & 0x1) {
            mounted_ = mount_host_dir(host_mount_path_);
        } else {
            mounted_ = false;
        }
        break;
    default:
        break;
    }
}

bool VirtIOFileSystemDevice::mount_host_dir(const std::string& host_path) {
    if (host_path.empty()) return false;
    host_mount_path_ = host_path;


    mounted_ = true;
    return true;
}

uint32_t VirtIOFileSystemDevice::handle_9p_version(size_t size) {


    return 0x30303230;  
}

uint32_t VirtIOFileSystemDevice::handle_9p_attach(size_t size) {

    return 0;  
}

uint32_t VirtIOFileSystemDevice::handle_9p_open(size_t size) {

    return 0;  
}

std::mutex LBFSBackend::mount_mutex_;

bool LBFSBackend::mount(const std::string& host_dir, const std::string& guest_mount_point) {
    std::lock_guard<std::mutex> lock(mount_mutex_);


    return true;
}

bool LBFSBackend::umount(const std::string& guest_mount_point) {
    std::lock_guard<std::mutex> lock(mount_mutex_);


    return true;
}

};  
