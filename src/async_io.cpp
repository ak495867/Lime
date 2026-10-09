#include "lime/async_io.hpp"
#include "lime/storage.hpp"

#include <algorithm>
#include <cstring>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>  // must precede windows.h
#include <windows.h>
#endif

#if defined(__linux__) && __has_include(<liburing.h>)
#define LIME_HAVE_IO_URING 1
#include <fcntl.h>
#include <liburing.h>
#include <unistd.h>
#endif

namespace lime {

// ---------------------------------------------------------------------------
// Shared completion queue plumbing used by every backend: worker/OS events are
// translated into (AsyncCompletion, Callback) pairs; poll() is the only place
// that ever invokes user callbacks.
// ---------------------------------------------------------------------------
namespace {

struct DoneItem {
    AsyncCompletion completion;
    AsyncIOEngine::Callback callback;
};

class QueueBase : public AsyncIOEngine {
public:
    size_t poll(size_t max_completions) override {
        size_t fired = 0;
        while (fired < max_completions) {
            DoneItem item;
            {
                std::lock_guard<std::mutex> lock(done_mutex_);
                if (done_queue_.empty()) break;
                item = std::move(done_queue_.front());
                done_queue_.pop_front();
            }
            delivered_.fetch_add(1);
            if (item.callback) item.callback(item.completion);
            fired++;
        }
        return fired;
    }

protected:
    void push_done(const AsyncCompletion& c, AsyncIOEngine::Callback cb) {
        std::lock_guard<std::mutex> lock(done_mutex_);
        done_queue_.push_back(DoneItem{c, std::move(cb)});
        completed_.fetch_add(1);
    }

    std::mutex done_mutex_;
    std::deque<DoneItem> done_queue_;
};

}  // namespace

// ---------------------------------------------------------------------------
// Windows backend: IOCP + overlapped ReadFile/WriteFile.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
namespace {

struct IocpRequest {
    OVERLAPPED ovl{};  // must be first: recovered via CONTAINING_RECORD
    AsyncOpType op{AsyncOpType::READ};
    void* buffer{nullptr};
    size_t bytes{0};
    uint64_t id{0};
    AsyncIOEngine::Callback cb;
};

class IOCPAsyncIOEngine final : public QueueBase {
public:
    ~IOCPAsyncIOEngine() override { shutdown(); }

    bool open(const std::string& path) override {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (is_open_) return true;

        HANDLE file = CreateFileA(path.c_str(),
                                  GENERIC_READ | GENERIC_WRITE,
                                  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                  nullptr, OPEN_EXISTING,
                                  FILE_FLAG_OVERLAPPED | FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;

        HANDLE iocp = CreateIoCompletionPort(file, nullptr, 0, 1);
        if (!iocp) {
            CloseHandle(file);
            return false;
        }

        file_ = file;
        iocp_ = iocp;
        stop_.store(false);
        worker_ = std::thread([this]() { worker_loop(); });
        is_open_ = true;
        return true;
    }

    bool is_open() const override { return is_open_; }

    bool submit(AsyncOpType op, uint64_t offset, void* buffer, size_t bytes, Callback cb) override {
        if (!is_open_ || !buffer || bytes == 0) return false;

        IocpRequest* req = new IocpRequest();
        req->op = op;
        req->buffer = buffer;
        req->bytes = bytes;
        req->id = next_id_.fetch_add(1);
        req->cb = std::move(cb);
        req->ovl.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
        req->ovl.OffsetHigh = static_cast<DWORD>(offset >> 32);

        BOOL ok;
        if (op == AsyncOpType::READ) {
            ok = ReadFile(file_, buffer, static_cast<DWORD>(bytes), nullptr, &req->ovl);
        } else {
            ok = WriteFile(file_, buffer, static_cast<DWORD>(bytes), nullptr, &req->ovl);
        }

        if (!ok) {
            DWORD err = GetLastError();
            if (err != ERROR_IO_PENDING) {
                // Rejected synchronously: complete it through the queue so the
                // "callbacks only fire from poll()" contract still holds.
                AsyncCompletion c{op, req->id, false, 0, static_cast<int>(err)};
                push_done(c, std::move(req->cb));
                delete req;
                submitted_.fetch_add(1);
                return true;
            }
        }

        submitted_.fetch_add(1);
        return true;
    }

    size_t pending() const override {
        uint64_t sub = submitted_.load();
        uint64_t del = delivered_.load();
        return sub > del ? static_cast<size_t>(sub - del) : 0;
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!is_open_ && !worker_.joinable()) return;
        is_open_ = false;
        stop_.store(true);
        if (iocp_) PostQueuedCompletionStatus(iocp_, 0, 0, nullptr);
        if (worker_.joinable()) worker_.join();
        if (iocp_) {
            CloseHandle(iocp_);
            iocp_ = nullptr;
        }
        if (file_ != INVALID_HANDLE_VALUE) {
            CloseHandle(file_);
            file_ = INVALID_HANDLE_VALUE;
        }
    }

    const char* backend_name() const override { return "IOCP"; }

private:
    void worker_loop() {
        while (!stop_.load()) {
            DWORD bytes = 0;
            ULONG_PTR key = 0;
            OVERLAPPED* ovl = nullptr;
            BOOL ok = GetQueuedCompletionStatus(iocp_, &bytes, &key, &ovl, 200);
            if (!ovl) continue;  // timeout or wake-up ping

            IocpRequest* req = CONTAINING_RECORD(ovl, IocpRequest, ovl);
            AsyncCompletion c;
            c.op = req->op;
            c.id = req->id;
            c.success = (ok != FALSE);
            c.bytes = c.success ? bytes : 0;
            c.error = c.success ? 0 : static_cast<int>(GetLastError());

            push_done(c, std::move(req->cb));
            delete req;
        }
    }

    std::mutex lifecycle_mutex_;
    std::atomic<bool> stop_{false};
    HANDLE file_{INVALID_HANDLE_VALUE};
    HANDLE iocp_{nullptr};
    std::thread worker_;
    bool is_open_{false};
};

}  // namespace
#endif  // _WIN32

// ---------------------------------------------------------------------------
// Linux backend: io_uring (when liburing is available).
// ---------------------------------------------------------------------------
#if defined(LIME_HAVE_IO_URING)
namespace {

struct UringRequest {
    AsyncOpType op;
    void* buffer;
    size_t bytes;
    uint64_t id;
    AsyncIOEngine::Callback cb;
};

class URingAsyncIOEngine final : public QueueBase {
public:
    ~URingAsyncIOEngine() override { shutdown(); }

    bool open(const std::string& path) override {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (is_open_) return true;

        if (io_uring_queue_init(64, &ring_, 0) != 0) return false;

        fd_ = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
        if (fd_ < 0) {
            io_uring_queue_exit(&ring_);
            return false;
        }
        is_open_ = true;
        return true;
    }

    bool is_open() const override { return is_open_; }

    bool submit(AsyncOpType op, uint64_t offset, void* buffer, size_t bytes, Callback cb) override {
        if (!is_open_ || !buffer || bytes == 0) return false;

        io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
        if (!sqe) {
            // Submission queue full: harvest a batch and retry once.
            poll(64);
            sqe = io_uring_get_sqe(&ring_);
            if (!sqe) return false;
        }

        UringRequest* req = new UringRequest{op, buffer, bytes, next_id_.fetch_add(1), std::move(cb)};
        if (op == AsyncOpType::READ) {
            io_uring_prep_read(sqe, fd_, buffer, bytes, static_cast<off_t>(offset));
        } else {
            io_uring_prep_write(sqe, fd_, buffer, bytes, static_cast<off_t>(offset));
        }
        io_uring_sqe_set_data(sqe, req);
        submitted_.fetch_add(1);

        int rc = io_uring_submit(&ring_);
        if (rc < 0) {
            AsyncCompletion c{op, req->id, false, 0, -rc};
            push_done(c, std::move(req->cb));
            delete req;
            return true;
        }
        return true;
    }

    size_t poll(size_t max_completions) override {
        if (!is_open_) return 0;
        size_t fired = 0;
        while (fired < max_completions) {
            io_uring_cqe* cqe = nullptr;
            if (io_uring_peek_cqe(&ring_, &cqe) != 0) break;
            UringRequest* req = static_cast<UringRequest*>(io_uring_cqe_get_data(cqe));

            AsyncCompletion c;
            c.op = req->op;
            c.id = req->id;
            c.success = (cqe->res >= 0);
            c.bytes = c.success ? static_cast<size_t>(cqe->res) : 0;
            c.error = c.success ? 0 : -cqe->res;

            AsyncIOEngine::Callback cb = std::move(req->cb);
            delete req;
            io_uring_cqe_seen(&ring_, cqe);
            delivered_.fetch_add(1);
            if (cb) cb(c);
            fired++;
        }
        return fired;
    }

    size_t pending() const override {
        uint64_t sub = submitted_.load();
        uint64_t del = delivered_.load();
        return sub > del ? static_cast<size_t>(sub - del) : 0;
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!is_open_) return;
        poll(1024);
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        io_uring_queue_exit(&ring_);
        is_open_ = false;
    }

    const char* backend_name() const override { return "io_uring"; }

private:
    std::mutex lifecycle_mutex_;
    io_uring ring_{};
    int fd_{-1};
    bool is_open_{false};
};

}  // namespace
#endif  // LIME_HAVE_IO_URING

// ---------------------------------------------------------------------------
// Portable fallback: dedicated worker thread performing paged file I/O.
// ---------------------------------------------------------------------------
namespace {

struct PoolRequest {
    AsyncOpType op;
    uint64_t offset;
    void* buffer;
    size_t bytes;
    uint64_t id;
    AsyncIOEngine::Callback cb;
};

class ThreadPoolAsyncIOEngine final : public QueueBase {
public:
    ~ThreadPoolAsyncIOEngine() override { shutdown(); }

    bool open(const std::string& path) override {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (is_open_) return true;
        path_ = path;
        stop_.store(false);
        worker_ = std::thread([this]() { worker_loop(); });
        is_open_ = true;
        return true;
    }

    bool is_open() const override { return is_open_; }

    bool submit(AsyncOpType op, uint64_t offset, void* buffer, size_t bytes, Callback cb) override {
        if (!is_open_ || !buffer || bytes == 0) return false;
        {
            std::lock_guard<std::mutex> lock(req_mutex_);
            requests_.push_back(PoolRequest{op, offset, buffer, bytes, next_id_.fetch_add(1), std::move(cb)});
        }
        submitted_.fetch_add(1);
        cv_.notify_one();
        return true;
    }

    size_t pending() const override {
        uint64_t sub = submitted_.load();
        uint64_t del = delivered_.load();
        return sub > del ? static_cast<size_t>(sub - del) : 0;
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lock(lifecycle_mutex_);
        if (!is_open_ && !worker_.joinable()) return;
        is_open_ = false;
        stop_.store(true);
        cv_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

    const char* backend_name() const override { return "ThreadPool"; }

private:
    void worker_loop() {
        std::fstream file(path_, std::ios::in | std::ios::out | std::ios::binary);
        while (true) {
            PoolRequest req;
            {
                std::unique_lock<std::mutex> lock(req_mutex_);
                cv_.wait(lock, [this]() { return !requests_.empty() || stop_.load(); });
                if (stop_.load() && requests_.empty()) return;
                req = std::move(requests_.front());
                requests_.pop_front();
            }
            if (!file.is_open()) {
                push_done(AsyncCompletion{req.op, req.id, false, 0, -1}, std::move(req.cb));
                continue;
            }

            AsyncCompletion c;
            c.op = req.op;
            c.id = req.id;
            file.clear();
            file.seekg(static_cast<std::streamoff>(req.offset), std::ios::beg);
            if (req.op == AsyncOpType::READ) {
                file.read(reinterpret_cast<char*>(req.buffer), static_cast<std::streamsize>(req.bytes));
                c.success = static_cast<size_t>(file.gcount()) == req.bytes;
                c.bytes = c.success ? req.bytes : static_cast<size_t>(file.gcount());
            } else {
                file.write(reinterpret_cast<const char*>(req.buffer), static_cast<std::streamsize>(req.bytes));
                c.success = static_cast<bool>(file);
                c.bytes = c.success ? req.bytes : 0;
            }
            c.error = c.success ? 0 : -1;
            push_done(c, std::move(req.cb));
        }
    }

    std::mutex lifecycle_mutex_;
    std::mutex req_mutex_;
    std::condition_variable cv_;
    std::deque<PoolRequest> requests_;
    std::string path_;
    std::atomic<bool> stop_{false};
    std::thread worker_;
    bool is_open_{false};
};

}  // namespace

std::shared_ptr<AsyncIOEngine> AsyncIOEngine::create_best() {
#if defined(_WIN32)
    return std::make_shared<IOCPAsyncIOEngine>();
#elif defined(LIME_HAVE_IO_URING)
    return std::make_shared<URingAsyncIOEngine>();
#else
    return std::make_shared<ThreadPoolAsyncIOEngine>();
#endif
}

// ---------------------------------------------------------------------------
// Sparse-disk scatter/gather submission.
// ---------------------------------------------------------------------------
bool submit_sparse_range(const std::shared_ptr<AsyncIOEngine>& engine,
                         const std::shared_ptr<SparseDisk>& disk,
                         AsyncOpType op, uint64_t byte_offset,
                         void* buffer, size_t bytes,
                         AsyncIOEngine::Callback cb) {
    if (!engine || !engine->is_open() || !disk || !disk->is_open()) return false;
    if (bytes == 0) return true;
    if (op == AsyncOpType::WRITE && !disk->preallocate_range(byte_offset, bytes)) return false;

    std::vector<SparseDisk::DiskRange> ranges;
    if (!disk->map_range(byte_offset, bytes, ranges)) return false;

    struct Aggregator {
        std::atomic<size_t> remaining{0};
        std::atomic<bool> success{true};
        AsyncOpType op{AsyncOpType::READ};
        size_t bytes{0};
        AsyncIOEngine::Callback cb;
    };

    auto agg = std::make_shared<Aggregator>();
    agg->op = op;
    agg->bytes = bytes;
    agg->cb = std::move(cb);

    uint8_t* base = static_cast<uint8_t*>(buffer);
    size_t io_segments = 0;

    for (const auto& r : ranges) {
        size_t dst = static_cast<size_t>(r.virtual_offset - byte_offset);
        if (!r.allocated) {
            if (op == AsyncOpType::READ) {
                std::memset(base + dst, 0, r.length);  // sparse hole reads as zeros
            }
            continue;
        }
        io_segments++;
        agg->remaining.fetch_add(1);
        bool ok = engine->submit(op, r.file_offset, base + dst, r.length,
                                 [agg](const AsyncCompletion& c) {
                                     if (!c.success) agg->success.store(false);
                                     if (agg->remaining.fetch_sub(1) == 1) {
                                         AsyncCompletion done{agg->op, 0, agg->success.load(), agg->bytes, 0};
                                         if (agg->cb) agg->cb(done);
                                     }
                                 });
        if (!ok) {
            if (agg->remaining.fetch_sub(1) == 1) {
                AsyncCompletion done{agg->op, 0, false, agg->bytes, -1};
                if (agg->cb) agg->cb(done);
            }
            return true;
        }
    }

    if (io_segments == 0) {
        // Entirely inside sparse holes: no backing I/O required.
        AsyncCompletion done{op, 0, true, bytes, 0};
        if (agg->cb) agg->cb(done);
    }
    return true;
}

}  // namespace lime
