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

struct PortForward {
    uint16_t host_port;
    uint16_t guest_port;
    std::string proto{"tcp"};
};

class UserNetStack {
public:
    UserNetStack();
    explicit UserNetStack(std::shared_ptr<VirtIONetDevice> net_dev);
    ~UserNetStack() = default;

    void add_port_forward(uint16_t host_port, uint16_t guest_port, const std::string& proto = "tcp");
    const std::vector<PortForward>& port_forwards() const { return forwards_; }

    // Process outgoing frame from guest and generate reply (if any)
    std::vector<uint8_t> process_frame(const std::vector<uint8_t>& frame);

    // Helpers to craft packets
    static std::vector<uint8_t> make_arp_reply(const uint8_t sender_mac[6], const uint8_t sender_ip[4],
                                               const uint8_t target_mac[6], const uint8_t target_ip[4]);
    static std::vector<uint8_t> make_icmp_echo_reply(const std::vector<uint8_t>& request_frame);
    static std::vector<uint8_t> make_dhcp_reply(const std::vector<uint8_t>& request_frame, bool is_ack);

private:
    std::shared_ptr<VirtIONetDevice> net_dev_;
    std::vector<PortForward> forwards_;
    mutable std::mutex mutex_;
    uint8_t guest_mac_[6]{0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
    uint8_t gateway_mac_[6]{0x52, 0x54, 0x00, 0x12, 0x34, 0x02};
    uint8_t gateway_ip_[4]{10, 0, 2, 2};
    uint8_t guest_ip_[4]{10, 0, 2, 15};
    uint8_t dns_ip_[4]{10, 0, 2, 3};
};

}

#endif
