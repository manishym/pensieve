#pragma once

#include <coroutine>
#include <cstdint>
#include <functional>
#include <liburing.h>
#include <netinet/in.h>

#include "common/types.h"
#include "io/io_uring_context.h"

namespace pensieve {

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

private:
    IoUringContext& ctx_;
    PrepFn prep_;
    Completion completion_;
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

}  // namespace pensieve
