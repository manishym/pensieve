#pragma once

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <functional>
#include <liburing.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include "common/types.h"
#include "io/io_uring_context.h"

namespace pensieve {

// ---- Generic awaitable (for simple io_uring ops) ----------------------------

class IoAwaitable {
public:
    using PrepFn = std::function<void(io_uring_sqe*)>;

    IoAwaitable(IoUringContext& ctx, PrepFn prep)
        : ctx_(ctx), prep_(std::move(prep)) {}

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        completion_ = Completion::from_coroutine(h);

        io_uring_sqe* sqe = ctx_.get_sqe();
        prep_(sqe);
        io_uring_sqe_set_data(sqe, &completion_);
        ctx_.submit();
    }

    int32_t await_resume() const noexcept { return completion_.result; }

    Completion* completion_ptr() { return &completion_; }

private:
    IoUringContext& ctx_;
    PrepFn prep_;
    Completion completion_;
};

// ---- UDP recv (owns msghdr + iovec across co_await) -------------------------

class UdpRecvAwaitable {
public:
    UdpRecvAwaitable(IoUringContext& ctx, fd_t fd, void* buf, size_t len,
                     sockaddr_in* src_addr)
        : ctx_(ctx), fd_(fd) {
        iov_.iov_base = buf;
        iov_.iov_len = len;
        std::memset(&msg_, 0, sizeof(msg_));
        msg_.msg_name = src_addr;
        msg_.msg_namelen = src_addr ? sizeof(sockaddr_in) : 0;
        msg_.msg_iov = &iov_;
        msg_.msg_iovlen = 1;
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        completion_ = Completion::from_coroutine(h);
        io_uring_sqe* sqe = ctx_.get_sqe();
        io_uring_prep_recvmsg(sqe, fd_, &msg_, 0);
        io_uring_sqe_set_data(sqe, &completion_);
        ctx_.submit();
    }

    int32_t await_resume() const noexcept { return completion_.result; }

    Completion* completion_ptr() { return &completion_; }

private:
    IoUringContext& ctx_;
    fd_t fd_;
    Completion completion_{};
    struct iovec iov_{};
    struct msghdr msg_{};
};

// ---- UDP send (owns msghdr + iovec + dest addr copy) ------------------------

class UdpSendAwaitable {
public:
    UdpSendAwaitable(IoUringContext& ctx, fd_t fd, const void* buf, size_t len,
                     const sockaddr_in& dest_addr)
        : ctx_(ctx), fd_(fd), dest_addr_(dest_addr) {
        iov_.iov_base = const_cast<void*>(buf);
        iov_.iov_len = len;
        std::memset(&msg_, 0, sizeof(msg_));
        msg_.msg_name = &dest_addr_;
        msg_.msg_namelen = sizeof(dest_addr_);
        msg_.msg_iov = &iov_;
        msg_.msg_iovlen = 1;
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        completion_ = Completion::from_coroutine(h);
        io_uring_sqe* sqe = ctx_.get_sqe();
        io_uring_prep_sendmsg(sqe, fd_, &msg_, 0);
        io_uring_sqe_set_data(sqe, &completion_);
        ctx_.submit();
    }

    int32_t await_resume() const noexcept { return completion_.result; }

private:
    IoUringContext& ctx_;
    fd_t fd_;
    Completion completion_{};
    sockaddr_in dest_addr_;
    struct iovec iov_{};
    struct msghdr msg_{};
};

// ---- Timeout (owns __kernel_timespec) ---------------------------------------

class TimeoutAwaitable {
public:
    TimeoutAwaitable(IoUringContext& ctx, std::chrono::milliseconds ms)
        : ctx_(ctx) {
        ts_.tv_sec = static_cast<long long>(ms.count() / 1000);
        ts_.tv_nsec = static_cast<long long>((ms.count() % 1000) * 1000000);
    }

    bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        completion_ = Completion::from_coroutine(h);
        io_uring_sqe* sqe = ctx_.get_sqe();
        io_uring_prep_timeout(sqe, &ts_, 0, 0);
        io_uring_sqe_set_data(sqe, &completion_);
        ctx_.submit();
    }

    int32_t await_resume() const noexcept { return completion_.result; }

    Completion* completion_ptr() { return &completion_; }

private:
    IoUringContext& ctx_;
    Completion completion_{};
    __kernel_timespec ts_{};
};

inline IoAwaitable async_accept(IoUringContext& ctx, fd_t listen_fd,
                                sockaddr_in* addr, socklen_t* len) {
    return IoAwaitable(ctx, [listen_fd, addr, len](io_uring_sqe* sqe) {
        io_uring_prep_accept(sqe, listen_fd,
                             reinterpret_cast<sockaddr*>(addr), len, 0);
    });
}

inline IoAwaitable async_read(IoUringContext& ctx, fd_t fd, void* buf,
                              size_t len) {
    return IoAwaitable(ctx, [fd, buf, len](io_uring_sqe* sqe) {
        io_uring_prep_read(sqe, fd, buf, static_cast<unsigned>(len), 0);
    });
}

inline IoAwaitable async_write(IoUringContext& ctx, fd_t fd, const void* buf,
                               size_t len) {
    return IoAwaitable(
        ctx, [fd, buf, len](io_uring_sqe* sqe) {
            io_uring_prep_write(sqe, fd, buf, static_cast<unsigned>(len), 0);
        });
}

inline IoAwaitable async_read_fixed(IoUringContext& ctx, fd_t fd, void* buf,
                                    size_t len, uint16_t buf_idx) {
    return IoAwaitable(ctx, [fd, buf, len, buf_idx](io_uring_sqe* sqe) {
        io_uring_prep_read_fixed(sqe, fd, buf, static_cast<unsigned>(len), 0,
                                 buf_idx);
    });
}

inline IoAwaitable async_write_fixed(IoUringContext& ctx, fd_t fd,
                                     const void* buf, size_t len,
                                     uint16_t buf_idx) {
    return IoAwaitable(
        ctx, [fd, buf, len, buf_idx](io_uring_sqe* sqe) {
            io_uring_prep_write_fixed(sqe, fd, buf,
                                      static_cast<unsigned>(len), 0, buf_idx);
        });
}

inline IoAwaitable async_close(IoUringContext& ctx, fd_t fd) {
    return IoAwaitable(ctx, [fd](io_uring_sqe* sqe) {
        io_uring_prep_close(sqe, fd);
    });
}

inline IoAwaitable async_nop(IoUringContext& ctx) {
    return IoAwaitable(ctx, [](io_uring_sqe* sqe) {
        io_uring_prep_nop(sqe);
    });
}

// ---- UDP helpers ------------------------------------------------------------

inline UdpRecvAwaitable async_recvfrom(IoUringContext& ctx, fd_t fd,
                                       void* buf, size_t len,
                                       sockaddr_in* src_addr) {
    return UdpRecvAwaitable(ctx, fd, buf, len, src_addr);
}

inline UdpSendAwaitable async_sendto(IoUringContext& ctx, fd_t fd,
                                     const void* buf, size_t len,
                                     const sockaddr_in& dest_addr) {
    return UdpSendAwaitable(ctx, fd, buf, len, dest_addr);
}

// ---- Timer ------------------------------------------------------------------

inline TimeoutAwaitable async_timeout(IoUringContext& ctx,
                                      std::chrono::milliseconds ms) {
    return TimeoutAwaitable(ctx, ms);
}

// ---- TCP connect ------------------------------------------------------------

inline IoAwaitable async_connect(IoUringContext& ctx, fd_t fd,
                                 const sockaddr_in& addr) {
    return IoAwaitable(ctx, [fd, addr](io_uring_sqe* sqe) {
        io_uring_prep_connect(sqe, fd,
                              reinterpret_cast<const sockaddr*>(&addr),
                              sizeof(addr));
    });
}

// ---- Cancel a pending SQE by its user_data pointer --------------------------

inline IoAwaitable async_cancel(IoUringContext& ctx, void* user_data) {
    return IoAwaitable(ctx, [user_data](io_uring_sqe* sqe) {
        io_uring_prep_cancel(sqe, user_data, 0);
    });
}

}  // namespace pensieve
