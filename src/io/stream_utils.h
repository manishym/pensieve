#pragma once

#include <cstddef>
#include <cstdint>

#include "io/awaitable.h"
#include "io/task.h"

namespace pensieve {

// Read exactly `len` bytes from `fd` into `buf`, issuing multiple
// async_read calls as needed.  Returns false on EOF or error.
inline Task<bool> read_exact(IoUringContext& ctx, fd_t fd,
                             void* buf, size_t len) {
    auto* dst = static_cast<uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        int32_t n = co_await async_read(ctx, fd, dst, remaining);
        if (n <= 0) co_return false;
        dst += n;
        remaining -= static_cast<size_t>(n);
    }
    co_return true;
}

// Write exactly `len` bytes from `buf` to `fd`, issuing multiple
// async_write calls as needed.  Returns false on error.
inline Task<bool> write_all(IoUringContext& ctx, fd_t fd,
                            const void* buf, size_t len) {
    auto* src = static_cast<const uint8_t*>(buf);
    size_t remaining = len;
    while (remaining > 0) {
        int32_t n = co_await async_write(ctx, fd, src, remaining);
        if (n <= 0) co_return false;
        src += n;
        remaining -= static_cast<size_t>(n);
    }
    co_return true;
}

}  // namespace pensieve
