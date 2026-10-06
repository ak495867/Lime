#include "lime/devices.hpp"
#include <iostream>
#include <thread>
#include <condition_variable>

namespace lime {

UartConsoleDevice::UartConsoleDevice(uint64_t base_addr) : base_addr_(base_addr) {}

uint32_t UartConsoleDevice::read(uint64_t offset, size_t) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (offset) {
    case 0x00:
        if (!rx_queue_.empty()) {
            uint8_t ch = rx_queue_.front();
            rx_queue_.pop();
            return ch;
        }
        return 0;
    case 0x01:
        return ier_;
    case 0x03:
        return lcr_;
    case 0x05: {
        uint8_t lsr = 0x20 | 0x40;
        if (!rx_queue_.empty()) {
            lsr |= 0x01;
        }
        return lsr;
    }
    default:
        return 0;
    }
}

void UartConsoleDevice::write(uint64_t offset, uint32_t value, size_t) {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (offset) {
    case 0x00: {
        char ch = static_cast<char>(value & 0xFF);
        tx_buffer_.push_back(ch);
        std::cout << ch << std::flush;
        break;
    }
    case 0x01:
        ier_ = static_cast<uint8_t>(value);
        break;
    case 0x03:
        lcr_ = static_cast<uint8_t>(value);
        break;
    case 0x04:
        mcr_ = static_cast<uint8_t>(value);
        break;
    case 0x07:
        scr_ = static_cast<uint8_t>(value);
        break;
    default:
        break;
    }
}

void UartConsoleDevice::tick() {}

void UartConsoleDevice::push_input(char ch) {
    std::lock_guard<std::mutex> lock(mutex_);
    rx_queue_.push(static_cast<uint8_t>(ch));
}

std::string UartConsoleDevice::get_output_buffer() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string res = tx_buffer_;
    tx_buffer_.clear();
    return res;
}

VirtIOBlockDevice::VirtIOBlockDevice(std::shared_ptr<SparseDisk> disk, uint64_t base_addr)
    : disk_(disk), base_addr_(base_addr) {}

VirtIOBlockDevice::~VirtIOBlockDevice() {
    stop_worker();
}

void VirtIOBlockDevice::start_worker() {
    if (worker_running_.load()) return;
    worker_running_ = true;
    worker_thread_ = std::thread([this]() {
        while (worker_running_.load()) {
            std::unique_lock<std::mutex> lock(req_mutex_);
            cv_.wait(lock, [this]() {
                return !pending_requests_.empty() || !worker_running_.load();
            });
            if (!worker_running_.load()) break;
            if (pending_requests_.empty()) continue;

            AsyncRequest req = pending_requests_.front();
            pending_requests_.pop();
            lock.unlock();

            bool ok = false;
            if (req.op == AsyncRequest::OpType::READ) {
                ok = disk_->read_sectors(req.lba, req.sector_count, req.buffer);
            } else {
                ok = disk_->write_sectors(req.lba, req.sector_count, req.buffer);
            }
            if (req.callback) req.callback(ok);
        }
    });
}

void VirtIOBlockDevice::stop_worker() {
    if (!worker_running_.load()) return;
    worker_running_ = false;
    cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void VirtIOBlockDevice::submit_async_read(uint64_t lba, uint32_t sector_count, void* buffer, std::function<void(bool)> callback) {
    std::lock_guard<std::mutex> lock(req_mutex_);
    pending_requests_.push({AsyncRequest::OpType::READ, lba, sector_count, buffer, callback});
    cv_.notify_one();
}

void VirtIOBlockDevice::submit_async_write(uint64_t lba, uint32_t sector_count, const void* buffer, std::function<void(bool)> callback) {
    std::lock_guard<std::mutex> lock(req_mutex_);
    pending_requests_.push({AsyncRequest::OpType::WRITE, lba, sector_count, const_cast<void*>(buffer), callback});
    cv_.notify_one();
}

uint32_t VirtIOBlockDevice::read(uint64_t offset, size_t) {
    switch (offset) {
    case 0x00: return 0x74726976;
    case 0x04: return 2;
    case 0x08: return 2;
    case 0x70: return status_;
    case 0x80: return disk_ ? static_cast<uint32_t>(disk_->capacity_bytes() / 512) : 0;
    default: return 0;
    }
}

void VirtIOBlockDevice::write(uint64_t offset, uint32_t value, size_t) {
    switch (offset) {
    case 0x70:
        status_ = value;
        break;
    case 0x84:
        current_lba_ = (current_lba_ & 0xFFFFFFFF00000000ULL) | value;
        break;
    case 0x88:
        current_lba_ = (current_lba_ & 0x00000000FFFFFFFFULL) | (static_cast<uint64_t>(value) << 32);
        break;
    case 0x8C:
        sector_count_ = value;
        break;
    default:
        break;
    }
}

VirtIONetDevice::VirtIONetDevice(uint64_t base_addr) : base_addr_(base_addr) {}

VirtIONetDevice::~VirtIONetDevice() {
    stop_worker();
}

void VirtIONetDevice::start_worker() {
    if (worker_running_.load()) return;
    worker_running_ = true;
    worker_thread_ = std::thread([this]() {
        while (worker_running_.load()) {
            std::unique_lock<std::mutex> lock(frame_mutex_);
            cv_.wait(lock, [this]() {
                return !pending_frames_.empty() || !worker_running_.load();
            });
            if (!worker_running_.load()) break;
            if (pending_frames_.empty()) continue;

            AsyncFrame frame = pending_frames_.front();
            pending_frames_.pop();
            lock.unlock();

            bool ok = true;
            if (ok) {
                packets_sent_++;
                packets_recv_++;  
            }
            if (frame.callback) frame.callback(ok);
        }
    });
}

void VirtIONetDevice::stop_worker() {
    if (!worker_running_.load()) return;
    worker_running_ = false;
    cv_.notify_all();
    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }
}

void VirtIONetDevice::send_frame_async(const std::vector<uint8_t>& frame, std::function<void(bool)> callback) {
    std::lock_guard<std::mutex> lock(frame_mutex_);
    pending_frames_.push({frame, callback});
    cv_.notify_one();
}

uint32_t VirtIONetDevice::read(uint64_t offset, size_t) {
    switch (offset) {
    case 0x00: return 0x74726976;
    case 0x04: return 2;
    case 0x08: return 1;
    case 0x70: return status_;
    case 0x80: return static_cast<uint32_t>(packets_sent_);
    case 0x84: return static_cast<uint32_t>(packets_recv_);
    default: return 0;
    }
}

void VirtIONetDevice::write(uint64_t offset, uint32_t value, size_t) {
    switch (offset) {
    case 0x70:
        status_ = value;
        break;
    case 0x88:
        packets_sent_++;
        packets_recv_++;
        break;
    default:
        break;
    }
}

VirtIOBalloonDevice::VirtIOBalloonDevice(std::shared_ptr<MemoryManager> mem, uint64_t base_addr)
    : mem_(mem), base_addr_(base_addr) {}

uint32_t VirtIOBalloonDevice::read(uint64_t offset, size_t) {
    switch (offset) {
    case 0x00: return 0x74726976;
    case 0x04: return 2;
    case 0x08: return 5;
    case 0x10: return num_pages_target_;
    case 0x14: return actual_pages_;
    default: return 0;
    }
}

void VirtIOBalloonDevice::write(uint64_t offset, uint32_t value, size_t) {
    switch (offset) {
    case 0x10:
        num_pages_target_ = value;
        if (mem_) {
            if (num_pages_target_ > actual_pages_) {
                size_t diff = num_pages_target_ - actual_pages_;
                size_t inf = mem_->inflate_balloon(diff);
                actual_pages_ += static_cast<uint32_t>(inf);
            } else if (num_pages_target_ < actual_pages_) {
                size_t diff = actual_pages_ - num_pages_target_;
                size_t def = mem_->deflate_balloon(diff);
                if (actual_pages_ >= def) {
                    actual_pages_ -= static_cast<uint32_t>(def);
                }
            }
        }
        break;
    default:
        break;
    }
}

VirtIOGraphicsDevice::VirtIOGraphicsDevice(uint64_t base_addr) : base_addr_(base_addr) {
    framebuffer_.resize(width_ * height_, 0);
}

uint32_t VirtIOGraphicsDevice::read(uint64_t offset, size_t) {
    if (offset >= 0x1000) {
        uint64_t fb_idx = (offset - 0x1000) / 4;
        if (fb_idx < framebuffer_.size()) return framebuffer_[fb_idx];
        return 0;
    }
    switch (offset) {
    case 0x00: return 0x74726976;
    case 0x04: return 2;
    case 0x08: return 16;
    case 0x10: return width_;
    case 0x14: return height_;
    default: return 0;
    }
}

void VirtIOGraphicsDevice::write(uint64_t offset, uint32_t value, size_t) {
    if (offset >= 0x1000) {
        uint64_t fb_idx = (offset - 0x1000) / 4;
        if (fb_idx < framebuffer_.size()) framebuffer_[fb_idx] = value;
        return;
    }
    switch (offset) {
    case 0x10:
        width_ = value;
        framebuffer_.resize(width_ * height_, 0);
        break;
    case 0x14:
        height_ = value;
        framebuffer_.resize(width_ * height_, 0);
        break;
    default:
        break;
    }
}

void DeviceBus::register_device(std::shared_ptr<Device> dev) {
    std::lock_guard<std::mutex> lock(mutex_);
    devices_.push_back(dev);
}

void DeviceBus::unregister_device(const std::string& name) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = std::remove_if(devices_.begin(), devices_.end(),
        [&name](const std::shared_ptr<Device>& dev) { return dev->name() == name; });
    devices_.erase(it, devices_.end());
}

std::shared_ptr<Device> DeviceBus::find_device(uint64_t gpa) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& dev : devices_) {
        uint64_t base = dev->base_address();
        uint64_t sz = dev->size();
        if (gpa >= base && gpa < base + sz) {
            return dev;
        }
    }
    return nullptr;
}

uint32_t DeviceBus::read(uint64_t gpa, size_t size) {
    auto dev = find_device(gpa);
    if (dev) {
        return dev->read(gpa - dev->base_address(), size);
    }
    return 0;
}

void DeviceBus::write(uint64_t gpa, uint32_t val, size_t size) {
    auto dev = find_device(gpa);
    if (dev) {
        dev->write(gpa - dev->base_address(), val, size);
    }
}

void DeviceBus::tick_all() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& dev : devices_) {
        dev->tick();
    }
}

const std::vector<std::shared_ptr<Device>>& DeviceBus::devices() const {
    return devices_;
}

}
