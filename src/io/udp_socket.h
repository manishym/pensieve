#pragma once

#include <cstdint>
#include <string>

#include "common/types.h"

namespace pensieve {

class UdpSocket {
public:
    UdpSocket(const std::string& host, uint16_t port);
    ~UdpSocket();

    UdpSocket(UdpSocket&& other) noexcept;
    UdpSocket& operator=(UdpSocket&& other) noexcept;

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    void close();
    fd_t fd() const { return fd_; }
    uint16_t port() const;

private:
    fd_t fd_ = -1;
};

}  // namespace pensieve
