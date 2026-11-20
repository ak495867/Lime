#include "lime/net_bridge.hpp"
#include <iostream>
#include <cstring>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>
#include <fcntl.h>
#endif

namespace lime {

HostNetBridge::HostNetBridge(std::shared_ptr<VirtIONetDevice> net_dev) : net_dev_(net_dev) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

HostNetBridge::~HostNetBridge() {
    stop_bridge();
#if defined(_WIN32)
    WSACleanup();
#endif
}

bool HostNetBridge::start_bridge(uint16_t listen_port) {
    std::lock_guard<std::mutex> lock(mutex_);
    port_ = listen_port;
    
    sock_ = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock_ == static_cast<uint64_t>(-1)) return false;

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port_);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (::bind(sock_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
#if defined(_WIN32)
        closesocket(sock_);
#else
        close(static_cast<int>(sock_));
#endif
        sock_ = 0;
        return false;
    }

#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(sock_, FIONBIO, &mode);
#else
    int flags = fcntl(sock_, F_GETFL, 0);
    fcntl(sock_, F_SETFL, flags | O_NONBLOCK);
#endif

    is_active_ = true;
    return true;
}

void HostNetBridge::stop_bridge() {
    std::lock_guard<std::mutex> lock(mutex_);
    is_active_ = false;
    if (sock_ != 0) {
#if defined(_WIN32)
        closesocket(sock_);
#else
        close(sock_);
#endif
        sock_ = 0;
    }
}

bool HostNetBridge::send_frame(const std::vector<uint8_t>& frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_active_ || sock_ == 0 || frame.empty()) return false;
    
    sockaddr_in dest{};
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port_ + 1);
    dest.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    
    int sent = ::sendto(sock_, reinterpret_cast<const char*>(frame.data()),
                        static_cast<int>(frame.size()), 0,
                        reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    return sent == static_cast<int>(frame.size());
}

std::vector<uint8_t> HostNetBridge::receive_frame() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!is_active_ || sock_ == 0) return {};
    
    std::vector<uint8_t> buf(2048);
    sockaddr_in sender{};
    socklen_t len = sizeof(sender);
    
    int n = ::recvfrom(sock_, reinterpret_cast<char*>(buf.data()), buf.size(), 0,
                       reinterpret_cast<sockaddr*>(&sender), &len);
    if (n > 0) {
        buf.resize(n);
        return buf;
    }
    return {};
}

bool HostNetBridge::is_active() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return is_active_;
}

UserNetStack::UserNetStack() {}

UserNetStack::UserNetStack(std::shared_ptr<VirtIONetDevice> net_dev) : net_dev_(net_dev) {
    if (net_dev_) {
        net_dev_->set_tx_handler([this](const std::vector<uint8_t>& frame) {
            auto reply = process_frame(frame);
            if (!reply.empty() && net_dev_) {
                net_dev_->receive_frame(reply);
            }
        });
    }
}

void UserNetStack::add_port_forward(uint16_t host_port, uint16_t guest_port, const std::string& proto) {
    std::lock_guard<std::mutex> lock(mutex_);
    forwards_.push_back({host_port, guest_port, proto});
}

std::vector<uint8_t> UserNetStack::make_arp_reply(const uint8_t sender_mac[6], const uint8_t sender_ip[4],
                                                  const uint8_t target_mac[6], const uint8_t target_ip[4]) {
    std::vector<uint8_t> reply(42, 0);
    // Ethernet header
    std::memcpy(&reply[0], target_mac, 6); // Dest MAC
    std::memcpy(&reply[6], sender_mac, 6); // Src MAC
    reply[12] = 0x08; reply[13] = 0x06;    // EtherType ARP

    // ARP payload
    reply[14] = 0x00; reply[15] = 0x01;    // HW Type: Ethernet
    reply[16] = 0x08; reply[17] = 0x00;    // Proto: IPv4
    reply[18] = 6;                         // HW size
    reply[19] = 4;                         // Proto size
    reply[20] = 0x00; reply[21] = 0x02;    // Opcode: Reply (2)
    std::memcpy(&reply[22], sender_mac, 6);
    std::memcpy(&reply[28], sender_ip, 4);
    std::memcpy(&reply[32], target_mac, 6);
    std::memcpy(&reply[38], target_ip, 4);
    return reply;
}

std::vector<uint8_t> UserNetStack::make_icmp_echo_reply(const std::vector<uint8_t>& req) {
    if (req.size() < 34) return {};
    std::vector<uint8_t> reply = req;
    // Swap Ethernet MAC
    std::memcpy(&reply[0], &req[6], 6);
    std::memcpy(&reply[6], &req[0], 6);

    // Swap IPv4 addresses
    std::memcpy(&reply[26], &req[30], 4);
    std::memcpy(&reply[30], &req[26], 4);

    uint8_t ihl = req[14] & 0x0F;
    size_t icmp_offset = 14 + ihl * 4;
    if (req.size() < icmp_offset + 8) return {};

    reply[icmp_offset] = 0; // Echo Reply type 0
    reply[icmp_offset + 1] = 0; // Code 0

    // Recompute ICMP checksum
    reply[icmp_offset + 2] = 0;
    reply[icmp_offset + 3] = 0;
    uint32_t csum = 0;
    size_t icmp_len = reply.size() - icmp_offset;
    for (size_t i = 0; i < icmp_len; i += 2) {
        uint16_t word = reply[icmp_offset + i] << 8;
        if (i + 1 < icmp_len) word |= reply[icmp_offset + i + 1];
        csum += word;
    }
    while (csum >> 16) csum = (csum & 0xFFFF) + (csum >> 16);
    uint16_t final_csum = ~csum;
    reply[icmp_offset + 2] = static_cast<uint8_t>(final_csum >> 8);
    reply[icmp_offset + 3] = static_cast<uint8_t>(final_csum & 0xFF);

    return reply;
}

std::vector<uint8_t> UserNetStack::make_dhcp_reply(const std::vector<uint8_t>& req, bool is_ack) {
    if (req.size() < 282) return {};

    std::vector<uint8_t> reply(342, 0);
    // Ethernet header
    const uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    const uint8_t gw_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x02};
    std::memcpy(&reply[0], bcast, 6);
    std::memcpy(&reply[6], gw_mac, 6);
    reply[12] = 0x08; reply[13] = 0x00; // IPv4

    // IP header
    reply[14] = 0x45; // Version 4, IHL 5
    uint16_t total_len = 342 - 14;
    reply[16] = static_cast<uint8_t>(total_len >> 8);
    reply[17] = static_cast<uint8_t>(total_len & 0xFF);
    reply[22] = 64;   // TTL
    reply[23] = 17;   // Protocol UDP
    reply[26] = 10; reply[27] = 0; reply[28] = 2; reply[29] = 2; // Src IP 10.0.2.2
    reply[30] = 255; reply[31] = 255; reply[32] = 255; reply[33] = 255; // Dst IP 255.255.255.255

    // UDP header
    reply[34] = 0x00; reply[35] = 67; // Src port 67 (bootps)
    reply[36] = 0x00; reply[37] = 68; // Dst port 68 (bootpc)
    uint16_t udp_len = total_len - 20;
    reply[38] = static_cast<uint8_t>(udp_len >> 8);
    reply[39] = static_cast<uint8_t>(udp_len & 0xFF);

    // BOOTP / DHCP payload (offset 42)
    size_t b_off = 42;
    reply[b_off + 0] = 2; // op: BOOTREPLY
    reply[b_off + 1] = 1; // htype: Ethernet
    reply[b_off + 2] = 6; // hlen
    reply[b_off + 3] = 0; // hops

    // Copy xid from request (offset 42 + 4 in req)
    std::memcpy(&reply[b_off + 4], &req[42 + 4], 4);

    // yiaddr: 10.0.2.15
    reply[b_off + 16] = 10; reply[b_off + 17] = 0; reply[b_off + 18] = 2; reply[b_off + 19] = 15;
    // siaddr: 10.0.2.2
    reply[b_off + 20] = 10; reply[b_off + 21] = 0; reply[b_off + 22] = 2; reply[b_off + 23] = 2;

    // chaddr: client MAC from req
    std::memcpy(&reply[b_off + 28], &req[b_off + 28], 6);

    // Magic cookie: 0x63, 0x82, 0x53, 0x63
    size_t opt_off = b_off + 236;
    reply[opt_off + 0] = 0x63; reply[opt_off + 1] = 0x82; reply[opt_off + 2] = 0x53; reply[opt_off + 3] = 0x63;
    opt_off += 4;

    // Option 53: Message type (2 = Offer, 5 = Ack)
    reply[opt_off++] = 53; reply[opt_off++] = 1; reply[opt_off++] = is_ack ? 5 : 2;

    // Option 54: Server identifier
    reply[opt_off++] = 54; reply[opt_off++] = 4;
    reply[opt_off++] = 10; reply[opt_off++] = 0; reply[opt_off++] = 2; reply[opt_off++] = 2;

    // Option 51: Lease time (86400s)
    reply[opt_off++] = 51; reply[opt_off++] = 4;
    reply[opt_off++] = 0x00; reply[opt_off++] = 0x01; reply[opt_off++] = 0x51; reply[opt_off++] = 0x80;

    // Option 1: Subnet mask (255.255.255.0)
    reply[opt_off++] = 1; reply[opt_off++] = 4;
    reply[opt_off++] = 255; reply[opt_off++] = 255; reply[opt_off++] = 255; reply[opt_off++] = 0;

    // Option 3: Router (10.0.2.2)
    reply[opt_off++] = 3; reply[opt_off++] = 4;
    reply[opt_off++] = 10; reply[opt_off++] = 0; reply[opt_off++] = 2; reply[opt_off++] = 2;

    // Option 6: DNS server (10.0.2.3)
    reply[opt_off++] = 6; reply[opt_off++] = 4;
    reply[opt_off++] = 10; reply[opt_off++] = 0; reply[opt_off++] = 2; reply[opt_off++] = 3;

    // Option 255: End
    reply[opt_off++] = 255;

    return reply;
}

std::vector<uint8_t> UserNetStack::process_frame(const std::vector<uint8_t>& frame) {
    if (frame.size() < 14) return {};
    uint16_t ethertype = (static_cast<uint16_t>(frame[12]) << 8) | frame[13];

    // ARP handling
    if (ethertype == 0x0806 && frame.size() >= 42) {
        uint16_t opcode = (static_cast<uint16_t>(frame[20]) << 8) | frame[21];
        if (opcode == 1) { // ARP Request
            const uint8_t* target_ip = &frame[38];
            if (target_ip[0] == 10 && target_ip[1] == 0 && target_ip[2] == 2) {
                const uint8_t* sender_mac = &frame[22];
                const uint8_t* sender_ip = &frame[28];
                const uint8_t* resp_mac = (target_ip[3] == 3) ? dns_ip_ : gateway_mac_;
                return make_arp_reply(resp_mac, target_ip, sender_mac, sender_ip);
            }
        }
        return {};
    }

    // IPv4 handling
    if (ethertype == 0x0800 && frame.size() >= 34) {
        uint8_t proto = frame[23];
        if (proto == 1) { // ICMP
            uint8_t ihl = frame[14] & 0x0F;
            size_t icmp_offset = 14 + ihl * 4;
            if (frame.size() >= icmp_offset + 8 && frame[icmp_offset] == 8) { // Echo request
                return make_icmp_echo_reply(frame);
            }
        } else if (proto == 17 && frame.size() >= 42) { // UDP
            uint16_t dst_port = (static_cast<uint16_t>(frame[36]) << 8) | frame[37];
            if (dst_port == 67 && frame.size() >= 282) { // DHCP request to server
                // Determine if Discover (Option 53 = 1) or Request (Option 53 = 3)
                bool is_ack = false;
                size_t opt_start = 42 + 240;
                for (size_t i = opt_start; i + 2 < frame.size() && frame[i] != 255;) {
                    uint8_t opt = frame[i];
                    uint8_t len = frame[i + 1];
                    if (opt == 53 && len >= 1) {
                        uint8_t msg_type = frame[i + 2];
                        is_ack = (msg_type == 3); // DHCPREQUEST -> ACK
                        break;
                    }
                    i += 2 + len;
                }
                return make_dhcp_reply(frame, is_ack);
            }
        }
    }

    return {};
}

}
