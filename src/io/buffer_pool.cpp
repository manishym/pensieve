#include "io/buffer_pool.h"

#include <cstdlib>
#include <cstring>
#include <stdexcept>

namespace pensieve {

BufferPool::BufferPool(IoUringContext& ctx, size_t buffer_size,
                       size_t buffer_count)
    : ctx_(ctx),
      buffer_size_(buffer_size),
      buffer_count_(buffer_count) {
    region_size_ = buffer_size_ * buffer_count_;

    region_ = std::aligned_alloc(4096, region_size_);
    if (!region_) {
        throw std::runtime_error("aligned_alloc failed for buffer pool");
    }
    std::memset(region_, 0, region_size_);

    iovecs_.resize(buffer_count_);
    for (size_t i = 0; i < buffer_count_; ++i) {
        auto* base = static_cast<uint8_t*>(region_) + i * buffer_size_;
        iovecs_[i].iov_base = base;
        iovecs_[i].iov_len = buffer_size_;
    }

    int ret = io_uring_register_buffers(ctx_.raw(), iovecs_.data(),
                                        static_cast<unsigned>(buffer_count_));
    if (ret < 0) {
        std::free(region_);
        region_ = nullptr;
        throw std::runtime_error(
            std::string("io_uring_register_buffers failed: ") + strerror(-ret));
    }

    free_list_.reserve(buffer_count_);
    for (size_t i = buffer_count_; i > 0; --i) {
        free_list_.push_back(static_cast<uint16_t>(i - 1));
    }
}

BufferPool::~BufferPool() {
    if (region_) {
        io_uring_unregister_buffers(ctx_.raw());
        std::free(region_);
    }
}

std::optional<BufferPool::BufferHandle> BufferPool::acquire() {
    if (free_list_.empty()) {
        return std::nullopt;
    }

    uint16_t idx = free_list_.back();
    free_list_.pop_back();

    return BufferHandle{
        .index = idx,
        .data = static_cast<uint8_t*>(iovecs_[idx].iov_base),
        .size = buffer_size_,
    };
}

void BufferPool::release(uint16_t index) {
    free_list_.push_back(index);
}

}  // namespace pensieve
