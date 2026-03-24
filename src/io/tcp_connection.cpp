#include "io/tcp_connection.h"

#include <unistd.h>

namespace pensieve {

TcpConnection::TcpConnection(IoUringContext& ctx, fd_t fd)
    : ctx_(&ctx), fd_(fd) {}

TcpConnection::~TcpConnection() = default;

TcpConnection::TcpConnection(TcpConnection&& other) noexcept
    : ctx_(other.ctx_), fd_(other.fd_) {
    other.fd_ = -1;
}

TcpConnection& TcpConnection::operator=(TcpConnection&& other) noexcept {
    if (this != &other) {
        close();
        ctx_ = other.ctx_;
        fd_ = other.fd_;
        other.fd_ = -1;
    }
    return *this;
}

namespace {

Completion* make_owned_completion(std::function<void(int32_t)> cb) {
    auto* owned_cb = new std::function<void(int32_t)>(std::move(cb));
    auto* comp = new Completion(Completion::from_callback(owned_cb));
    comp->owns_callback = true;
    return comp;
}

}  // namespace

void TcpConnection::async_read(void* buf, size_t len,
                                std::function<void(int32_t)> cb) {
    auto* comp = make_owned_completion(std::move(cb));
    io_uring_sqe* sqe = ctx_->get_sqe();
    io_uring_prep_read(sqe, fd_, buf, static_cast<unsigned>(len), 0);
    io_uring_sqe_set_data(sqe, comp);
    ctx_->submit();
}

void TcpConnection::async_write(const void* buf, size_t len,
                                 std::function<void(int32_t)> cb) {
    auto* comp = make_owned_completion(std::move(cb));
    io_uring_sqe* sqe = ctx_->get_sqe();
    io_uring_prep_write(sqe, fd_, buf, static_cast<unsigned>(len), 0);
    io_uring_sqe_set_data(sqe, comp);
    ctx_->submit();
}

void TcpConnection::async_read_fixed(void* buf, size_t len, uint16_t buf_idx,
                                      std::function<void(int32_t)> cb) {
    auto* comp = make_owned_completion(std::move(cb));
    io_uring_sqe* sqe = ctx_->get_sqe();
    io_uring_prep_read_fixed(sqe, fd_, buf, static_cast<unsigned>(len), 0,
                             buf_idx);
    io_uring_sqe_set_data(sqe, comp);
    ctx_->submit();
}

void TcpConnection::async_write_fixed(const void* buf, size_t len,
                                       uint16_t buf_idx,
                                       std::function<void(int32_t)> cb) {
    auto* comp = make_owned_completion(std::move(cb));
    io_uring_sqe* sqe = ctx_->get_sqe();
    io_uring_prep_write_fixed(sqe, fd_, buf, static_cast<unsigned>(len), 0,
                              buf_idx);
    io_uring_sqe_set_data(sqe, comp);
    ctx_->submit();
}

void TcpConnection::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

}  // namespace pensieve
