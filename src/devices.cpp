#include "lime/devices.hpp"
#include <algorithm>
#include "lime/async_io.hpp"
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

VirtIOBlockDevice::VirtIOBlockDevice(std::shared_ptr<BlockStorage> disk, std::shared_ptr<MemoryManager> mem, uint64_t base_addr)
    : disk_(disk), mem_(mem), base_addr_(base_addr) {}

VirtIOBlockDevice::~VirtIOBlockDevice() {
    stop_worker();
}

void VirtIOBlockDevice::start_worker() {
    if (worker_running_.load()) return;
    worker_running_ = true;
    worker_thread_ = std::thread([this]() {
        while (true) {
            std::unique_lock<std::mutex> lock(req_mutex_);
            cv_.wait(lock, [this]() {
                return !pending_requests_.empty() || !worker_running_.load();
            });
            if (pending_requests_.empty()) {
                if (!worker_running_.load()) break;
                continue;
            }

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
    auto sparse_disk = std::dynamic_pointer_cast<SparseDisk>(disk_);
    if (async_engine_ && async_engine_->is_open() && sparse_disk) {
        auto cb = callback;
        if (submit_sparse_range(async_engine_, sparse_disk, AsyncOpType::READ,
                            lba * SparseDisk::SECTOR_SIZE, buffer,
                            static_cast<size_t>(sector_count) * SparseDisk::SECTOR_SIZE,
                            [cb](const AsyncCompletion& c) { if (cb) cb(c.success); })) return;
    }
    std::lock_guard<std::mutex> lock(req_mutex_);
    pending_requests_.push({AsyncRequest::OpType::READ, lba, sector_count, buffer, callback});
    cv_.notify_one();
}

void VirtIOBlockDevice::submit_async_write(uint64_t lba, uint32_t sector_count, const void* buffer, std::function<void(bool)> callback) {
    auto sparse_disk = std::dynamic_pointer_cast<SparseDisk>(disk_);
    if (async_engine_ && async_engine_->is_open() && sparse_disk) {
        auto cb = callback;
        if (submit_sparse_range(async_engine_, sparse_disk, AsyncOpType::WRITE,
                            lba * SparseDisk::SECTOR_SIZE, const_cast<void*>(buffer),
                            static_cast<size_t>(sector_count) * SparseDisk::SECTOR_SIZE,
                            [cb](const AsyncCompletion& c) { if (cb) cb(c.success); })) return;
    }
    std::lock_guard<std::mutex> lock(req_mutex_);
    pending_requests_.push({AsyncRequest::OpType::WRITE, lba, sector_count, const_cast<void*>(buffer), callback});
    cv_.notify_one();
}

void VirtIOBlockDevice::attach_async_engine(std::shared_ptr<AsyncIOEngine> engine) {
    async_engine_ = std::move(engine);
}

void VirtIOBlockDevice::tick() {
    if (async_engine_) {
        async_engine_->poll(16);
    }
}

void VirtIOBlockDevice::process_virtqueue() {
    if (!mem_ || !disk_ || vq_pfn_ == 0) return;

    // Layout of split virtqueue:
    // Descriptors: vq_addr
    // Avail ring:   vq_addr + queue_num_ * 16
    // Used ring:    align_up(avail_end, queue_align_)
    uint64_t vq_addr = static_cast<uint64_t>(vq_pfn_) * queue_align_;
    uint64_t desc_table_addr = vq_addr;
    uint64_t avail_addr = desc_table_addr + static_cast<uint64_t>(queue_num_) * 16;
    uint64_t avail_end = avail_addr + 4 + static_cast<uint64_t>(queue_num_) * 2;
    uint64_t used_addr = (avail_end + queue_align_ - 1) & ~(static_cast<uint64_t>(queue_align_) - 1);

    uint16_t avail_idx = mem_->read16(avail_addr + 2);

    while (last_avail_idx_ != avail_idx) {
        uint16_t ring_idx = last_avail_idx_ % queue_num_;
        uint16_t head_desc = mem_->read16(avail_addr + 4 + ring_idx * 2);

        uint16_t cur_desc = head_desc;
        uint32_t total_written = 0;
        uint8_t status = 0; // VIRTIO_BLK_S_OK

        struct VirtioBlkReq {
            uint32_t type;
            uint32_t reserved;
            uint64_t sector;
        } req_hdr{};

        bool have_req = false;
        uint64_t status_addr = 0;
        int safety_limit = queue_num_ * 2;

        while (safety_limit-- > 0) {
            uint64_t d_addr = desc_table_addr + cur_desc * 16;
            uint64_t buf_addr = mem_->read64(d_addr);
            uint32_t buf_len = mem_->read32(d_addr + 8);
            uint16_t flags = mem_->read16(d_addr + 12);
            uint16_t next = mem_->read16(d_addr + 14);

            if (!have_req && buf_len >= 16) {
                req_hdr.type = mem_->read32(buf_addr);
                req_hdr.reserved = mem_->read32(buf_addr + 4);
                req_hdr.sector = mem_->read64(buf_addr + 8);
                have_req = true;
            } else if (flags & 0x02) {
                // Device-writable buffer: data for READ or 1-byte status
                if (buf_len == 1 && (flags & 0x01) == 0) {
                    status_addr = buf_addr;
                } else if (req_hdr.type == 0) { // READ
                    uint32_t sectors = buf_len / 512;
                    std::vector<uint8_t> tmp(buf_len, 0);
                    if (sectors > 0) {
                        bool ok = disk_->read_sectors(req_hdr.sector, sectors, tmp.data());
                        if (!ok) status = 1; // VIRTIO_BLK_S_IOERR
                    }
                    mem_->write_bytes(buf_addr, tmp.data(), buf_len);
                    total_written += buf_len;
                    req_hdr.sector += sectors;
                } else {
                    status_addr = buf_addr;
                }
            } else {
                // Device-readable buffer: data for WRITE
                if (req_hdr.type == 1) { // WRITE
                    uint32_t sectors = buf_len / 512;
                    std::vector<uint8_t> tmp(buf_len);
                    mem_->read_bytes(buf_addr, tmp.data(), buf_len);
                    if (sectors > 0) {
                        bool ok = disk_->write_sectors(req_hdr.sector, sectors, tmp.data());
                        if (!ok) status = 1;
                    }
                    req_hdr.sector += sectors;
                }
            }

            if (!(flags & 0x01)) break;
            cur_desc = next;
        }

        if (status_addr != 0) {
            mem_->write8(status_addr, status);
            total_written += 1;
        }

        uint16_t used_idx = mem_->read16(used_addr + 2);
        uint16_t used_ring_entry = used_idx % queue_num_;
        uint64_t elem_addr = used_addr + 4 + used_ring_entry * 8;
        mem_->write32(elem_addr, head_desc);
        mem_->write32(elem_addr + 4, total_written);

        mem_->write16(used_addr + 2, used_idx + 1);
        last_avail_idx_++;
    }

    interrupt_status_ |= 0x1;
}

uint32_t VirtIOBlockDevice::read(uint64_t offset, size_t) {
    switch (offset) {
    case 0x00: return 0x74726976; // 'virt'
    case 0x04: return 2;          // Version 2
    case 0x08: return 2;          // Device ID 2 = Block
    case 0x0C: return 0x4c494d45; // Vendor ID 'LIME'
    case 0x10: return 0;          // HostFeatures
    case 0x34: return queue_num_; // QueueNumMax
    case 0x44: return (vq_pfn_ != 0) ? 1 : 0; // QueueReady
    case 0x60: return interrupt_status_;
    case 0x70: return status_;
    case 0x80:
    case 0x100: return disk_ ? static_cast<uint32_t>(disk_->capacity_bytes() / 512) : 0;
    case 0x84:
    case 0x104: return disk_ ? static_cast<uint32_t>((disk_->capacity_bytes() / 512) >> 32) : 0;
    default: return 0;
    }
}

void VirtIOBlockDevice::write(uint64_t offset, uint32_t value, size_t) {
    switch (offset) {
    case 0x14: /* HostFeaturesSel */ break;
    case 0x20: /* GuestFeatures */ break;
    case 0x24: /* GuestFeaturesSel */ break;
    case 0x28: /* GuestPageSize */ break;
    case 0x30: queue_sel_ = value; break;
    case 0x38: queue_num_ = (value != 0) ? value : 128; break;
    case 0x3c: queue_align_ = (value != 0) ? value : 4096; break;
    case 0x40:
        vq_pfn_ = value;
        break;
    case 0x50:
        if (value == 0 || value == queue_sel_) {
            process_virtqueue();
        }
        break;
    case 0x64:
        interrupt_status_ &= ~value;
        break;
    case 0x70:
        status_ = value;
        if (status_ == 0) {
            last_avail_idx_ = 0;
            interrupt_status_ = 0;
            vq_pfn_ = 0;
        }
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

VirtIONetDevice::VirtIONetDevice(std::shared_ptr<MemoryManager> mem, uint64_t base_addr) 
    : mem_(mem), base_addr_(base_addr) {}

VirtIONetDevice::~VirtIONetDevice() {
    stop_worker();
}

void VirtIONetDevice::start_worker() {
    if (worker_running_.load()) return;
    worker_running_ = true;
    worker_thread_ = std::thread([this]() {
        while (true) {
            std::unique_lock<std::mutex> lock(frame_mutex_);
            cv_.wait(lock, [this]() {
                return !pending_frames_.empty() || !worker_running_.load();
            });
            if (pending_frames_.empty()) {
                if (!worker_running_.load()) break;
                continue;
            }

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
    case 0x40:
        vq_pfn_ = value;
        break;
    case 0x50:
        if (mem_) {
            uint64_t vq_addr = static_cast<uint64_t>(vq_pfn_) * 4096;
            // Full implementation: read vring_avail, pop frames, send to bridge, push to vring_used
            cv_.notify_one();
        }
        break;
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

#if defined(_WIN32)
#include <windows.h>
#endif

namespace lime {

VirtIOGraphicsDevice::~VirtIOGraphicsDevice() {
    stop_display();
}

void VirtIOGraphicsDevice::start_display(std::shared_ptr<VirtIOInputDevice> input_dev) {
#if defined(_WIN32)
    if (display_running_) return;
    display_running_ = true;
    display_thread_ = std::thread([this, input_dev]() {
        WNDCLASS wc = {};
        wc.lpfnWndProc = DefWindowProc;
        wc.hInstance = GetModuleHandle(nullptr);
        wc.lpszClassName = "LIME_UI";
        RegisterClass(&wc);

        HWND hwnd = CreateWindowEx(
            0, "LIME_UI", "LIME Virtual Machine",
            WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
            width_, height_, nullptr, nullptr, wc.hInstance, nullptr
        );

        if (!hwnd) return;
        hwnd_ = hwnd;
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        HDC hdc = GetDC(hwnd);
        
        MSG msg;
        while (display_running_) {
            while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT || msg.message == WM_CLOSE) {
                    display_running_ = false;
                    break;
                }
                
                if (input_dev) {
                    if (msg.message == WM_KEYDOWN || msg.message == WM_KEYUP) {
                        uint16_t code = static_cast<uint16_t>(msg.wParam);
                        uint32_t val = (msg.message == WM_KEYDOWN) ? 1 : 0;
                        input_dev->push_event(1, code, val); // 1 = EV_KEY
                    } else if (msg.message == WM_MOUSEMOVE) {
                        int x = (int)(short)LOWORD(msg.lParam);
                        int y = (int)(short)HIWORD(msg.lParam);
                        input_dev->push_event(3, 0, x); // 3 = EV_ABS, 0 = ABS_X
                        input_dev->push_event(3, 1, y); // 3 = EV_ABS, 1 = ABS_Y
                    }
                }
                
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }
            
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bmi.bmiHeader.biWidth = width_;
            bmi.bmiHeader.biHeight = -static_cast<int>(height_); 
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            
            StretchDIBits(hdc, 0, 0, width_, height_, 0, 0, width_, height_,
                          framebuffer_.data(), &bmi, DIB_RGB_COLORS, SRCCOPY);
            
            std::this_thread::sleep_for(std::chrono::milliseconds(16)); // ~60 FPS
        }
        ReleaseDC(hwnd, hdc);
        DestroyWindow(hwnd);
    });
#endif
}

void VirtIOGraphicsDevice::stop_display() {
    display_running_ = false;
    if (display_thread_.joinable()) display_thread_.join();
}

VirtIOInputDevice::VirtIOInputDevice(uint64_t base_addr) : base_addr_(base_addr) {}

uint32_t VirtIOInputDevice::read(uint64_t offset, size_t) {
    switch (offset) {
    case 0x00: return 0x74726976;
    case 0x04: return 2;
    case 0x08: return 18; // virtio-input
    case 0x70: return status_;
    default: return 0;
    }
}

void VirtIOInputDevice::write(uint64_t offset, uint32_t value, size_t) {
    switch (offset) {
    case 0x70: status_ = value; break;
    default: break;
    }
}

void VirtIOInputDevice::push_event(uint16_t type, uint16_t code, uint32_t value) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push({type, code, value});
}

}
