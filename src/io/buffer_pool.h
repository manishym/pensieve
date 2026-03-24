#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <sys/uio.h>

#include "io/io_uring_context.h"

namespace pensieve {

class BufferPool {
public:
    struct BufferHandle {
        uint16_t index;
        uint8_t* data;
        size_t   size;
    };

    BufferPool(IoUringContext& ctx, size_t buffer_size, size_t buffer_count);
    ~BufferPool();

    BufferPool(const BufferPool&)            = delete;
    BufferPool& operator=(const BufferPool&) = delete;

    std::optional<BufferHandle> acquire();
    void release(uint16_t index);

    const struct iovec* iovecs() const { return iovecs_.data(); }
    size_t buffer_size() const { return buffer_size_; }
    size_t count() const { return buffer_count_; }
    size_t available() const { return free_list_.size(); }

private:
    IoUringContext& ctx_;
    size_t buffer_size_;
    size_t buffer_count_;
    void* region_ = nullptr;
    size_t region_size_ = 0;
    std::vector<struct iovec> iovecs_;
    std::vector<uint16_t> free_list_;
};

}  // namespace pensieve
