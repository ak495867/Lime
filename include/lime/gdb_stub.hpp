#ifndef LIME_GDB_STUB_HPP
#define LIME_GDB_STUB_HPP

#include <cstdint>
#include <string>
#include <memory>
#include <unordered_set>
#include <atomic>
#include <vector>

namespace lime {

class VCPU;
class MemoryManager;

class GDBStub {
public:
    GDBStub(std::shared_ptr<VCPU> vcpu, std::shared_ptr<MemoryManager> mem, uint16_t port);
    ~GDBStub();

    bool start();
    void stop();

    bool handle_client();

    bool has_breakpoint(uint64_t pc) const;
    void add_breakpoint(uint64_t pc);
    void remove_breakpoint(uint64_t pc);

    void notify_halt(int signal = 5);

    static uint8_t calculate_checksum(const std::string& data);
    static std::string format_packet(const std::string& data);
    static bool parse_packet(const std::string& raw, std::string& out_data);

    std::string process_command(const std::string& cmd);

    bool is_connected() const { return client_fd_ >= 0; }
    uint16_t port() const { return port_; }
    bool is_single_step() const { return single_step_; }
    void clear_single_step() { single_step_ = false; }

private:
    std::string read_packet();
    bool send_packet(const std::string& data);

    std::shared_ptr<VCPU> vcpu_;
    std::shared_ptr<MemoryManager> mem_;
    uint16_t port_;
    int server_fd_{-1};
    int client_fd_{-1};
    std::unordered_set<uint64_t> breakpoints_;
    std::atomic<bool> running_{false};
    bool single_step_{false};
};

} // namespace lime

#endif // LIME_GDB_STUB_HPP
