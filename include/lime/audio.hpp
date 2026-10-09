#ifndef LIME_AUDIO_HPP
#define LIME_AUDIO_HPP

#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <mutex>
#include "lime/devices.hpp"
#include "lime/memory.hpp"

namespace lime {

class VirtIOAudioDevice : public Device {
public:
    explicit VirtIOAudioDevice(uint64_t base_addr = 0x10006000);
    ~VirtIOAudioDevice() override = default;

    std::string name() const override { return "VirtIO-Audio"; }
    uint64_t base_address() const override { return base_addr_; }
    uint64_t size() const override { return 0x1000; }

    uint32_t read(uint64_t offset, size_t size) override;
    void write(uint64_t offset, uint32_t value, size_t size) override;

    bool play_audio(const std::vector<uint8_t>& audio_data);
    bool record_audio(std::vector<uint8_t>& audio_data);
    bool set_volume(uint8_t volume);

private:
    uint64_t base_addr_;
    uint32_t status_{0};
    uint32_t volume_{0x8000};  
    uint32_t sample_rate_{48000};
    uint32_t channels_{2};
    std::vector<uint8_t> audio_buffer_;
    mutable std::mutex mutex_;
};

}

#endif
