#include "io/io_uring_context.h"

#include <cstring>
#include <stdexcept>
#include <sys/eventfd.h>
#include <unistd.h>

namespace pensieve {

IoUringContext::IoUringContext(uint32_t queue_depth, uint32_t flags) {
    struct io_uring_params params{};
    params.flags = flags;

    int ret = io_uring_queue_init_params(queue_depth, &ring_, &params);
    if (ret < 0) {
        throw std::runtime_error(
            std::string("io_uring_queue_init failed: ") + strerror(-ret));
    }

    stop_fd_ = ::eventfd(0, EFD_NONBLOCK);
    if (stop_fd_ < 0) {
        io_uring_queue_exit(&ring_);
        throw std::runtime_error("eventfd() failed");
    }
}

IoUringContext::~IoUringContext() {
    if (stop_fd_ >= 0) ::close(stop_fd_);
    io_uring_queue_exit(&ring_);
}

io_uring_sqe* IoUringContext::get_sqe() {
    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (!sqe) {
        submit();
        sqe = io_uring_get_sqe(&ring_);
    }
    return sqe;
}

int IoUringContext::submit() {
    return io_uring_submit(&ring_);
}

int IoUringContext::submit_and_wait(unsigned wait_nr) {
    return io_uring_submit_and_wait(&ring_, wait_nr);
}

int IoUringContext::peek_cqe(io_uring_cqe** cqe) {
    return io_uring_peek_cqe(&ring_, cqe);
}

int IoUringContext::wait_cqe(io_uring_cqe** cqe) {
    return io_uring_wait_cqe(&ring_, cqe);
}

void IoUringContext::seen_cqe(io_uring_cqe* cqe) {
    io_uring_cqe_seen(&ring_, cqe);
}

void IoUringContext::process_cqe(io_uring_cqe* cqe) {
    auto* comp = reinterpret_cast<Completion*>(io_uring_cqe_get_data(cqe));
    if (!comp) return;

    comp->result = cqe->res;

    switch (comp->type) {
    case Completion::Type::Callback:
        if (comp->callback) {
            (*comp->callback)(cqe->res);
            if (comp->owns_callback) {
                delete comp->callback;
                delete comp;
            }
        }
        break;
    case Completion::Type::Coroutine:
        if (comp->coro_handle) {
            comp->coro_handle.resume();
        }
        break;
    }
}

void IoUringContext::arm_stop_event() {
    stop_completion_ = {};
    stop_completion_.type = Completion::Type::Callback;
    stop_completion_.callback = nullptr;

    io_uring_sqe* sqe = io_uring_get_sqe(&ring_);
    if (sqe) {
        io_uring_prep_read(sqe, stop_fd_, &stop_completion_.result,
                           sizeof(uint64_t), 0);
        io_uring_sqe_set_data(sqe, nullptr);
    }
}

void IoUringContext::run() {
    running_.store(true, std::memory_order_relaxed);
    arm_stop_event();

    while (running_.load(std::memory_order_relaxed)) {
        int ret = io_uring_submit_and_wait(&ring_, 1);
        if (ret < 0) {
            if (ret == -EINTR) continue;
            break;
        }

        io_uring_cqe* cqe = nullptr;
        unsigned head;
        unsigned count = 0;

        io_uring_for_each_cqe(&ring_, head, cqe) {
            process_cqe(cqe);
            ++count;
        }

        io_uring_cq_advance(&ring_, count);
    }
}

void IoUringContext::stop() {
    running_.store(false, std::memory_order_relaxed);
    uint64_t val = 1;
    ::write(stop_fd_, &val, sizeof(val));
}

}  // namespace pensieve
