#ifndef LIME_NET_BRIDGE_HPP
#define LIME_NET_BRIDGE_HPP

#include <cstdint>
#include <vector>
#include <string>
#include <mutex>
#include <memory>
#include "lime/devices.hpp"

namespace lime {

class HostNetBridge {
public:
    explicit HostNetBridge(std::shared_ptr<VirtIONetDevice> net_dev);
    ~HostNetBridge();

    bool start_bridge(uint16_t listen_port = 8888);
    void stop_bridge();

    bool send_frame(const std::vector<uint8_t>& frame);
    std::vector<uint8_t> receive_frame();

    bool is_active() const;

private:
    std::shared_ptr<VirtIONetDevice> net_dev_;
    bool is_active_{false};
    uint16_t port_{8888};
    mutable std::mutex mutex_;
    uint64_t sock_{0};
};

}

#endif
