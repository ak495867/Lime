#ifndef LIME_ASYNC_IO_HPP
#define LIME_ASYNC_IO_HPP

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace lime {

class SparseDisk;

enum class AsyncOpType { READ, WRITE };

struct AsyncCompletion {
    AsyncOpType op{AsyncOpType::READ};
    uint64_t id{0};
    bool success{false};
    size_t bytes{0};
    int error{0};
};

// ---------------------------------------------------------------------------
// AsyncIOEngine — pluggable asynchronous file-I/O backend.
//
// Contract:
//   * submit() never performs the I/O on the caller's thread: it only queues
//     the operation against the backend (IOCP / io_uring / worker pool) and
//     returns immediately.
//   * Completion callbacks are invoked exclusively from poll(), on whichever
//     thread calls it — so a vCPU thread can issue I/O, keep executing guest
//     instructions, and later harvest completions at a safe point.
// ---------------------------------------------------------------------------
class AsyncIOEngine {
public:
    using Callback = std::function<void(const AsyncCompletion&)>;
    virtual ~AsyncIOEngine() = default;

    virtual bool open(const std::string& path) = 0;
    virtual bool is_open() const = 0;
    virtual bool submit(AsyncOpType op, uint64_t offset, void* buffer, size_t bytes, Callback cb) = 0;

    // Invokes up to max_completions queued callbacks. Returns how many fired.
    virtual size_t poll(size_t max_completions = 64) = 0;

    // Number of accepted operations that have not yet been delivered.
    virtual size_t pending() const = 0;
    virtual void shutdown() = 0;
    virtual const char* backend_name() const = 0;

    uint64_t submitted() const { return submitted_.load(); }
    uint64_t completed() const { return completed_.load(); }
    uint64_t delivered() const { return delivered_.load(); }

    // IOCP on Windows, io_uring on Linux (when liburing is available),
    // worker-thread pool everywhere else.
    static std::shared_ptr<AsyncIOEngine> create_best();

protected:
    std::atomic<uint64_t> submitted_{0};
    std::atomic<uint64_t> completed_{0};
    std::atomic<uint64_t> delivered_{0};
    std::atomic<uint64_t> next_id_{1};
};

// Submit a byte range of a sparse disk through an async engine. Handles block
// mapping, on-demand allocation (writes), zero-fill for sparse holes (reads)
// and scatter/gather across the 64 KB blocks; `cb` fires exactly once when
// every backing-file segment of the range has completed.
bool submit_sparse_range(const std::shared_ptr<AsyncIOEngine>& engine,
                         const std::shared_ptr<SparseDisk>& disk,
                         AsyncOpType op, uint64_t byte_offset,
                         void* buffer, size_t bytes,
                         AsyncIOEngine::Callback cb);

}  // namespace lime

#endif  // LIME_ASYNC_IO_HPP
