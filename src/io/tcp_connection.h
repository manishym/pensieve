#pragma once

#include <cstdint>
#include <functional>

#include "common/types.h"
#include "io/io_uring_context.h"

namespace pensieve {

class TcpConnection {
public:
    TcpConnection(IoUringContext& ctx, fd_t fd);
    ~TcpConnection();

    TcpConnection(TcpConnection&& other) noexcept;
    TcpConnection& operator=(TcpConnection&& other) noexcept;

    TcpConnection(const TcpConnection&)            = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;

    void async_read(void* buf, size_t len,
                    std::function<void(int32_t)> cb);
    void async_write(const void* buf, size_t len,
                     std::function<void(int32_t)> cb);

    void async_read_fixed(void* buf, size_t len, uint16_t buf_idx,
                          std::function<void(int32_t)> cb);
    void async_write_fixed(const void* buf, size_t len, uint16_t buf_idx,
                           std::function<void(int32_t)> cb);

    void close();
    fd_t fd() const { return fd_; }

private:
    IoUringContext* ctx_;
    fd_t fd_;
};

}  // namespace pensieve
