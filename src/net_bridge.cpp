#include "lime/net_bridge.hpp"
#include <iostream>

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
    
    ::sendto(sock_, reinterpret_cast<const char*>(frame.data()), frame.size(), 0,
             reinterpret_cast<sockaddr*>(&dest), sizeof(dest));
    return true;
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

}
