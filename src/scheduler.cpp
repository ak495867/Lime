#include "lime/scheduler.hpp"

namespace lime {

ResourceScheduler::ResourceScheduler(std::shared_ptr<VCPU> vcpu,
                                       std::shared_ptr<MemoryManager> mem,
                                       AllocationPolicy policy)
    : vcpu_(vcpu), mem_(mem), policy_(policy) {}

ResourceScheduler::~ResourceScheduler() {
    stop();
}

void ResourceScheduler::start() {
    if (running_.exchange(true)) return;
    monitor_thread_ = std::thread(&ResourceScheduler::monitor_loop, this);
}

void ResourceScheduler::stop() {
    if (!running_.exchange(false)) return;
    if (monitor_thread_.joinable()) {
        monitor_thread_.join();
    }
}

void ResourceScheduler::set_policy(AllocationPolicy policy) {
    policy_ = policy;
}

AllocationPolicy ResourceScheduler::policy() const {
    return policy_.load();
}

void ResourceScheduler::notify_cycle(bool was_idle) {
    if (was_idle) {
        idle_cycles_++;
    } else {
        active_cycles_++;
    }
}

uint32_t ResourceScheduler::calculate_sleep_duration_ms() {
    uint64_t act = active_cycles_.load();
    uint64_t idl = idle_cycles_.load();
    uint64_t tot = act + idl;

    if (tot == 0) return 0;

    double idle_ratio = static_cast<double>(idl) / static_cast<double>(tot);
    AllocationPolicy p = policy_.load();

    uint32_t sleep_ms = 0;
    if (p == AllocationPolicy::AGGRESSIVE) {
        if (idle_ratio > 0.8) sleep_ms = 15;
        else if (idle_ratio > 0.5) sleep_ms = 5;
        else if (idle_ratio > 0.2) sleep_ms = 1;
    } else if (p == AllocationPolicy::BALANCED) {
        if (idle_ratio > 0.9) sleep_ms = 10;
        else if (idle_ratio > 0.6) sleep_ms = 3;
        else if (idle_ratio > 0.3) sleep_ms = 1;
    } else {
        if (idle_ratio > 0.95) sleep_ms = 5;
    }

    current_sleep_ms_ = sleep_ms;
    return sleep_ms;
}

ResourceMetrics ResourceScheduler::metrics() const {
    ResourceMetrics m;
    uint64_t act = active_cycles_.load();
    uint64_t idl = idle_cycles_.load();
    uint64_t tot = act + idl;

    if (tot > 0) {
        m.host_cpu_usage_percent = (static_cast<double>(act) / static_cast<double>(tot)) * 100.0;
    } else {
        m.host_cpu_usage_percent = 0.0;
    }

    if (mem_) {
        m.allocated_ram_bytes = mem_->allocated_bytes();
        m.reclaimed_ram_bytes = mem_->reclaimed_bytes();
    }

    m.vcpu_active_cycles = act;
    m.vcpu_idle_cycles = idl;
    m.current_sleep_ms = current_sleep_ms_.load();
    return m;
}

void ResourceScheduler::monitor_loop() {
    while (running_.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        tick_count_++;

        if (mem_) {
            AllocationPolicy p = policy_.load();
            uint64_t max_idle_ticks = 50;
            if (p == AllocationPolicy::AGGRESSIVE) {
                max_idle_ticks = 20;
            } else if (p == AllocationPolicy::LAZY) {
                max_idle_ticks = 200;
            }
            mem_->reclaim_idle_pages(tick_count_, max_idle_ticks);
        }
    }
}

}
