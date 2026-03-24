#pragma once

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <liburing.h>

#include "common/types.h"

namespace pensieve {

struct Completion {
    enum class Type : uint8_t { Callback, Coroutine };

    Type type = Type::Callback;
    bool owns_callback = false;
    int32_t result = 0;
    std::function<void(int32_t)>* callback = nullptr;
    std::coroutine_handle<>       coro_handle = nullptr;

    static Completion from_callback(std::function<void(int32_t)>* cb) {
        Completion c;
        c.type = Type::Callback;
        c.callback = cb;
        return c;
    }

    static Completion from_coroutine(std::coroutine_handle<> h) {
        Completion c;
        c.type = Type::Coroutine;
        c.coro_handle = h;
        return c;
    }
};

class IoUringContext {
public:
    explicit IoUringContext(uint32_t queue_depth = kDefaultQueueDepth,
                           uint32_t flags = 0);
    ~IoUringContext();

    IoUringContext(const IoUringContext&)            = delete;
    IoUringContext& operator=(const IoUringContext&) = delete;

    io_uring_sqe* get_sqe();
    int  submit();
    int  submit_and_wait(unsigned wait_nr);

    int  peek_cqe(io_uring_cqe** cqe);
    int  wait_cqe(io_uring_cqe** cqe);
    void seen_cqe(io_uring_cqe* cqe);

    void run();
    void stop();
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

    struct io_uring* raw() { return &ring_; }

private:
    void process_cqe(io_uring_cqe* cqe);
    void arm_stop_event();

    struct io_uring ring_{};
    std::atomic<bool> running_{false};
    fd_t stop_fd_ = -1;
    Completion stop_completion_;
};

}  // namespace pensieve
