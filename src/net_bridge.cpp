#include "lime/net_bridge.hpp"

namespace lime {

HostNetBridge::HostNetBridge(std::shared_ptr<VirtIONetDevice> net_dev) : net_dev_(net_dev) {}

HostNetBridge::~HostNetBridge() {
    stop_bridge();
}

bool HostNetBridge::start_bridge(uint16_t listen_port) {
    std::lock_guard<std::mutex> lock(mutex_);
    port_ = listen_port;
    is_active_ = true;
    return true;
}

void HostNetBridge::stop_bridge() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_active_ = false;
}

bool HostNetBridge::send_frame(const std::vector<uint8_t>&) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_active_) return false;
    return true;
}

std::vector<uint8_t> HostNetBridge::receive_frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    return {};
}

bool HostNetBridge::is_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_active_;
}

}
