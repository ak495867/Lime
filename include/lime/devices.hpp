#ifndef LIME_DEVICES_HPP
#define LIME_DEVICES_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <queue>
#include <mutex>
#include "lime/storage.hpp"
#include "lime/memory.hpp"

namespace lime {

class Device {
public:
    virtual ~Device() = default;
    virtual std::string name() const = 0;
    virtual uint64_t base_address() const = 0;
    virtual uint64_t size() const = 0;
    virtual uint32_t read(uint64_t offset, size_t size) = 0;
    virtual void write(uint64_t offset, uint32_t value, size_t size) = 0;
    virtual void tick() {}
};

class UartConsoleDevice : public Device {
public:
    explicit UartConsoleDevice(uint64_t base_addr = 0x10000000);
    ~UartConsoleDevice() override = default;

    std::string name() const override { return "UART16550"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x100; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;
    void tick() override;

    void push_input(char ch);
    std::string get_output_buffer();

private:
    uint64_t base_addr_;
    std::queue<uint8_t> rx_queue_;
    std::string tx_buffer_;
    mutable std::mutex mutex_;
    uint8_t ier_{0};
    uint8_t lcr_{0};
    uint8_t mcr_{0};
    uint8_t scr_{0};
};

class VirtIOBlockDevice : public Device {
public:
    VirtIOBlockDevice(std::shared_ptr<SparseDisk> disk, uint64_t base_addr = 0x10001000);
    ~VirtIOBlockDevice() override = default;

    std::string name() const override { return "VirtIO-Block"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x1000; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

private:
    std::shared_ptr<SparseDisk> disk_;
    uint64_t base_addr_;
    uint32_t status_{0};
    uint64_t current_lba_{0};
    uint32_t sector_count_{0};
};

class VirtIONetDevice : public Device {
public:
    explicit VirtIONetDevice(uint64_t base_addr = 0x10002000);
    ~VirtIONetDevice() override = default;

    std::string name() const override { return "VirtIO-Net"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x1000; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

    uint64_t packets_sent() const { return packets_sent_; }
    uint64_t packets_recv() const { return packets_recv_; }

private:
    uint64_t base_addr_;
    uint32_t status_{0};
    uint64_t packets_sent_{0};
    uint64_t packets_recv_{0};
};

class VirtIOBalloonDevice : public Device {
public:
    VirtIOBalloonDevice(std::shared_ptr<MemoryManager> mem, uint64_t base_addr = 0x10003000);
    ~VirtIOBalloonDevice() override = default;

    std::string name() const override { return "VirtIO-Balloon"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x1000; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

private:
    std::shared_ptr<MemoryManager> mem_;
    uint64_t base_addr_;
    uint32_t num_pages_target_{0};
    uint32_t actual_pages_{0};
};

class VirtIOGraphicsDevice : public Device {
public:
    explicit VirtIOGraphicsDevice(uint64_t base_addr = 0x10004000);
    ~VirtIOGraphicsDevice() override = default;

    std::string name() const override { return "VirtIO-GPU"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x1000; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    uint64_t base_addr_;
    uint32_t width_{320};
    uint32_t height_{240};
    std::vector<uint32_t> framebuffer_;
};

class DeviceBus {
public:
    DeviceBus() = default;
    ~DeviceBus() = default;

    void register_device(std::shared_ptr<Device> dev);
    void unregister_device(const std::string& name);
    std::shared_ptr<Device> find_device(uint64_t gpa);

    uint32_t read(uint64_t gpa, size_t size);
    void write(uint64_t gpa, uint32_t val, size_t size);
    void tick_all();

    const std::vector<std::shared_ptr<Device>>& devices() const;

private:
    std::vector<std::shared_ptr<Device>> devices_;
    mutable std::mutex mutex_;
};

}

#endif
