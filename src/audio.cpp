#include "lime/audio.hpp"

namespace lime {

VirtIOAudioDevice::VirtIOAudioDevice(uint64_t base_addr) : base_addr_(base_addr) {}

uint32_t VirtIOAudioDevice::read(uint64_t offset, size_t) {
    switch (offset) {
    case 0x00: return 0x74726976;  
    case 0x04: return 2;           
    case 0x08: return 1;           
    case 0x70: return status_;
    case 0x80: return volume_;
    case 0x84: return sample_rate_;
    case 0x88: return channels_;
    default: return 0;
    }
}

void VirtIOAudioDevice::write(uint64_t offset, uint32_t value, size_t) {
    switch (offset) {
    case 0x70:
        status_ = value;
        break;
    case 0x80:
        volume_ = value;
        break;
    case 0x84:
        sample_rate_ = value;
        break;
    case 0x88:
        channels_ = value;
        break;
    default:
        break;
    }
}

bool VirtIOAudioDevice::play_audio(const std::vector<uint8_t>& audio_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    audio_buffer_ = audio_data;
    status_ |= 0x1;  
    return true;
}

bool VirtIOAudioDevice::record_audio(std::vector<uint8_t>& audio_data) {
    std::lock_guard<std::mutex> lock(mutex_);
    return false;
}

bool VirtIOAudioDevice::set_volume(uint8_t volume) {
    volume_ = volume * 257;
    return true;
}

};  
