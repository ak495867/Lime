#include "lime/gdb_stub.hpp"
#include "lime/vcpu.hpp"
#include "lime/memory.hpp"

#include <iostream>
#include <iomanip>
#include <sstream>
#include <cstring>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#endif

namespace lime {

namespace {

void close_socket(int fd) {
    if (fd >= 0) {
#if defined(_WIN32)
        closesocket(fd);
#else
        close(fd);
#endif
    }
}

std::string byte_to_hex(uint8_t byte) {
    static const char hex_chars[] = "0123456789abcdef";
    std::string s;
    s += hex_chars[(byte >> 4) & 0x0F];
    s += hex_chars[byte & 0x0F];
    return s;
}

uint8_t hex_to_byte(char hi, char lo) {
    auto val = [](char c) -> uint8_t {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
    };
    return (val(hi) << 4) | val(lo);
}

std::string u64_to_gdb_hex(uint64_t v) {
    // Little-endian hex representation (8 bytes = 16 hex chars)
    std::string s;
    for (int i = 0; i < 8; ++i) {
        s += byte_to_hex(static_cast<uint8_t>((v >> (i * 8)) & 0xFF));
    }
    return s;
}

uint64_t gdb_hex_to_u64(const std::string& hex) {
    uint64_t v = 0;
    for (size_t i = 0; i < 8 && (i * 2 + 1) < hex.size(); ++i) {
        uint8_t b = hex_to_byte(hex[i * 2], hex[i * 2 + 1]);
        v |= (static_cast<uint64_t>(b) << (i * 8));
    }
    return v;
}

} // namespace

GDBStub::GDBStub(std::shared_ptr<VCPU> vcpu, std::shared_ptr<MemoryManager> mem, uint16_t port)
    : vcpu_(vcpu), mem_(mem), port_(port) {
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

GDBStub::~GDBStub() {
    stop();
#if defined(_WIN32)
    WSACleanup();
#endif
}

uint8_t GDBStub::calculate_checksum(const std::string& data) {
    uint8_t sum = 0;
    for (char c : data) {
        sum += static_cast<uint8_t>(c);
    }
    return sum;
}

std::string GDBStub::format_packet(const std::string& data) {
    uint8_t csum = calculate_checksum(data);
    return "$" + data + "#" + byte_to_hex(csum);
}

bool GDBStub::parse_packet(const std::string& raw, std::string& out_data) {
    size_t start = raw.find('$');
    if (start == std::string::npos) return false;
    size_t hash = raw.find('#', start);
    if (hash == std::string::npos || hash + 2 >= raw.size()) return false;

    std::string data = raw.substr(start + 1, hash - start - 1);
    uint8_t expected_csum = hex_to_byte(raw[hash + 1], raw[hash + 2]);
    if (calculate_checksum(data) != expected_csum) return false;

    out_data = data;
    return true;
}

bool GDBStub::start() {
    server_fd_ = static_cast<int>(socket(AF_INET, SOCK_STREAM, 0));
    if (server_fd_ < 0) return false;

    int opt = 1;
#if defined(_WIN32)
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
#else
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port_);

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close_socket(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 1) < 0) {
        close_socket(server_fd_);
        server_fd_ = -1;
        return false;
    }

    running_ = true;
    std::cout << "[GDB] Listening on port " << port_ << "..." << std::endl;
    return true;
}

void GDBStub::stop() {
    running_ = false;
    if (client_fd_ >= 0) {
        close_socket(client_fd_);
        client_fd_ = -1;
    }
    if (server_fd_ >= 0) {
        close_socket(server_fd_);
        server_fd_ = -1;
    }
}

bool GDBStub::send_packet(const std::string& data) {
    if (client_fd_ < 0) return false;
    std::string formatted = format_packet(data);
    int sent = ::send(client_fd_, formatted.c_str(), static_cast<int>(formatted.size()), 0);
    return sent == static_cast<int>(formatted.size());
}

std::string GDBStub::read_packet() {
    if (client_fd_ < 0) return "";
    std::string buffer;
    char c = 0;
    bool in_packet = false;

    while (running_) {
        int r = ::recv(client_fd_, &c, 1, 0);
        if (r <= 0) return "";

        if (c == '+') continue; // ACK
        if (c == 0x03) return "\x03"; // Interrupt (Ctrl+C from GDB)
        if (c == '$') {
            in_packet = true;
            buffer.clear();
            buffer += c;
        } else if (in_packet) {
            buffer += c;
            if (buffer.size() >= 3 && buffer[buffer.size() - 3] == '#') {
                // Send '+' ACK back
                char ack = '+';
                ::send(client_fd_, &ack, 1, 0);
                std::string data;
                if (parse_packet(buffer, data)) return data;
                return "";
            }
        }
    }
    return "";
}

bool GDBStub::handle_client() {
    if (server_fd_ < 0) return false;

    if (client_fd_ < 0) {
        sockaddr_in client_addr{};
#if defined(_WIN32)
        int len = sizeof(client_addr);
#else
        socklen_t len = sizeof(client_addr);
#endif
        client_fd_ = static_cast<int>(accept(server_fd_, reinterpret_cast<sockaddr*>(&client_addr), &len));
        if (client_fd_ < 0) return false;
        std::cout << "[GDB] Client connected." << std::endl;
    }

    while (running_) {
        std::string cmd = read_packet();
        if (cmd.empty()) {
            close_socket(client_fd_);
            client_fd_ = -1;
            return false;
        }

        if (cmd == "\x03") {
            // Halt request
            send_packet("S05");
            continue;
        }

        std::string resp = process_command(cmd);
        if (resp == "RESUME_CONT") {
            single_step_ = false;
            return true;
        }
        if (resp == "RESUME_STEP") {
            single_step_ = true;
            return true;
        }

        send_packet(resp);
    }

    return false;
}

std::string GDBStub::process_command(const std::string& cmd) {
    if (cmd.empty()) return "";

    char type = cmd[0];

    // Halt reason query
    if (type == '?') {
        return "S05";
    }

    // Supported features
    if (cmd.rfind("qSupported", 0) == 0) {
        return "PacketSize=1000;qXfer:features:read+";
    }

    // Read all registers (g)
    if (type == 'g') {
        std::string s;
        if (vcpu_) {
            for (size_t i = 0; i < 32; ++i) {
                s += u64_to_gdb_hex(vcpu_->get_reg(i));
            }
            s += u64_to_gdb_hex(vcpu_->get_pc());
        }
        return s;
    }

    // Write all registers (G)
    if (type == 'G' && vcpu_) {
        std::string hex = cmd.substr(1);
        for (size_t i = 0; i < 32 && (i + 1) * 16 <= hex.size(); ++i) {
            vcpu_->set_reg(i, gdb_hex_to_u64(hex.substr(i * 16, 16)));
        }
        if (hex.size() >= 33 * 16) {
            vcpu_->set_pc(gdb_hex_to_u64(hex.substr(32 * 16, 16)));
        }
        return "OK";
    }

    // Read single register (p)
    if (type == 'p' && vcpu_) {
        try {
            size_t reg_idx = std::stoul(cmd.substr(1), nullptr, 16);
            if (reg_idx < 32) return u64_to_gdb_hex(vcpu_->get_reg(reg_idx));
            if (reg_idx == 32) return u64_to_gdb_hex(vcpu_->get_pc());
        } catch (...) {}
        return "E01";
    }

    // Read memory: m <addr>,<length>
    if (type == 'm') {
        size_t comma = cmd.find(',');
        if (comma != std::string::npos && mem_) {
            try {
                uint64_t addr = std::stoull(cmd.substr(1, comma - 1), nullptr, 16);
                size_t len = std::stoul(cmd.substr(comma + 1), nullptr, 16);
                std::string s;
                for (size_t i = 0; i < len; ++i) {
                    s += byte_to_hex(mem_->read8(addr + i));
                }
                return s;
            } catch (...) {}
        }
        return "E01";
    }

    // Write memory: M <addr>,<length>:<data>
    if (type == 'M') {
        size_t comma = cmd.find(',');
        size_t colon = cmd.find(':');
        if (comma != std::string::npos && colon != std::string::npos && colon > comma && mem_) {
            try {
                uint64_t addr = std::stoull(cmd.substr(1, comma - 1), nullptr, 16);
                size_t len = std::stoul(cmd.substr(comma + 1, colon - comma - 1), nullptr, 16);
                std::string hex = cmd.substr(colon + 1);
                for (size_t i = 0; i < len && (i * 2 + 1) < hex.size(); ++i) {
                    uint8_t b = hex_to_byte(hex[i * 2], hex[i * 2 + 1]);
                    mem_->write8(addr + i, b);
                }
                return "OK";
            } catch (...) {}
        }
        return "E01";
    }

    // Single step: s
    if (type == 's') {
        return "RESUME_STEP";
    }

    // Continue: c
    if (type == 'c') {
        return "RESUME_CONT";
    }

    // Breakpoint: Z0 (insert) or z0 (remove)
    if (cmd.rfind("Z0,", 0) == 0) {
        size_t second_comma = cmd.find(',', 3);
        try {
            std::string addr_str = (second_comma != std::string::npos) ?
                cmd.substr(3, second_comma - 3) : cmd.substr(3);
            uint64_t addr = std::stoull(addr_str, nullptr, 16);
            add_breakpoint(addr);
            return "OK";
        } catch (...) { return "E01"; }
    }
    if (cmd.rfind("z0,", 0) == 0) {
        size_t second_comma = cmd.find(',', 3);
        try {
            std::string addr_str = (second_comma != std::string::npos) ?
                cmd.substr(3, second_comma - 3) : cmd.substr(3);
            uint64_t addr = std::stoull(addr_str, nullptr, 16);
            remove_breakpoint(addr);
            return "OK";
        } catch (...) { return "E01"; }
    }

    // Kill
    if (type == 'k') {
        return "OK";
    }

    return "";
}

bool GDBStub::has_breakpoint(uint64_t pc) const {
    return breakpoints_.find(pc) != breakpoints_.end();
}

void GDBStub::add_breakpoint(uint64_t pc) {
    breakpoints_.insert(pc);
}

void GDBStub::remove_breakpoint(uint64_t pc) {
    breakpoints_.erase(pc);
}

void GDBStub::notify_halt(int signal) {
    if (client_fd_ >= 0) {
        std::string s = "S" + byte_to_hex(static_cast<uint8_t>(signal));
        send_packet(s);
    }
}

} // namespace lime
