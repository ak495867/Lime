#ifndef LIME_SCHEDULER_HPP
#define LIME_SCHEDULER_HPP

#include <cstdint>
#include <atomic>
#include <thread>
#include <chrono>
#include <memory>
#include "lime/vcpu.hpp"
#include "lime/memory.hpp"

namespace lime {

enum class AllocationPolicy {
    AGGRESSIVE,
    BALANCED,
    LAZY
};

struct ResourceMetrics {
    double host_cpu_usage_percent{0.0};
    size_t allocated_ram_bytes{0};
    size_t reclaimed_ram_bytes{0};
    uint64_t vcpu_active_cycles{0};
    uint64_t vcpu_idle_cycles{0};
    uint32_t current_sleep_ms{0};
};

class ResourceScheduler {
public:
    ResourceScheduler(std::shared_ptr<VCPU> vcpu,
                      std::shared_ptr<MemoryManager> mem,
                      AllocationPolicy policy = AllocationPolicy::BALANCED);
    ~ResourceScheduler();

    void start();
    void stop();

    void set_policy(AllocationPolicy policy);
    AllocationPolicy policy() const;

    ResourceMetrics metrics() const;
    void notify_cycle(bool was_idle);

    uint32_t calculate_sleep_duration_ms();

private:
    void monitor_loop();

    std::shared_ptr<VCPU> vcpu_;
    std::shared_ptr<MemoryManager> mem_;
    std::atomic<AllocationPolicy> policy_{AllocationPolicy::BALANCED};

    std::atomic<bool> running_{false};
    std::thread monitor_thread_;

    std::atomic<uint64_t> active_cycles_{0};
    std::atomic<uint64_t> idle_cycles_{0};
    std::atomic<uint32_t> current_sleep_ms_{0};

    uint64_t tick_count_{0};
};

}

#endif
