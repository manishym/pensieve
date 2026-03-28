#include "sdk/connection_pool.h"

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

namespace pensieve {

namespace {

int tcp_connect(const std::string& host, uint16_t port) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), std::to_string(port).c_str(),
                    &hints, &result) != 0) {
        return -1;
    }
    int fd = socket(result->ai_family, result->ai_socktype,
                    result->ai_protocol);
    if (fd < 0) { freeaddrinfo(result); return -1; }

    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));

    if (connect(fd, result->ai_addr, result->ai_addrlen) < 0) {
        close(fd);
        freeaddrinfo(result);
        return -1;
    }
    freeaddrinfo(result);
    return fd;
}

bool send_all(int fd, const void* buf, size_t len) {
    auto* p = static_cast<const uint8_t*>(buf);
    while (len > 0) {
        ssize_t n = ::send(fd, p, len, MSG_NOSIGNAL);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

bool recv_all(int fd, void* buf, size_t len) {
    auto* p = static_cast<uint8_t*>(buf);
    while (len > 0) {
        ssize_t n = ::recv(fd, p, len, 0);
        if (n <= 0) return false;
        p += n;
        len -= static_cast<size_t>(n);
    }
    return true;
}

}  // namespace

ConnectionPool::ConnectionPool(size_t max_idle_per_node)
    : max_idle_(max_idle_per_node) {}

ConnectionPool::~ConnectionPool() {
    std::lock_guard lock(mu_);
    for (auto& [key, q] : pool_) {
        for (int fd : q) close(fd);
    }
}

int ConnectionPool::acquire(const NodeEndpoint& ep) {
    PeerKey key{ep.host, ep.data_port};
    {
        std::lock_guard lock(mu_);
        auto it = pool_.find(key);
        if (it != pool_.end() && !it->second.empty()) {
            int fd = it->second.front();
            it->second.pop_front();
            return fd;
        }
    }
    return tcp_connect(ep.host, ep.data_port);
}

void ConnectionPool::release(const NodeEndpoint& ep, int fd) {
    PeerKey key{ep.host, ep.data_port};
    std::lock_guard lock(mu_);
    auto& q = pool_[key];
    if (q.size() >= max_idle_) {
        close(fd);
        return;
    }
    q.push_back(fd);
}

void ConnectionPool::discard(int fd) {
    close(fd);
}

bool ConnectionPool::send_request(int fd, const Request& req) {
    auto wire = serialize_request(req);
    return send_all(fd, wire.data(), wire.size());
}

std::optional<Response> ConnectionPool::recv_response(int fd) {
    MemHeader hdr{};
    if (!recv_all(fd, &hdr, sizeof(hdr))) return std::nullopt;

    if (hdr.magic != 0x81) return std::nullopt; // Must be Response magic

    uint32_t body_len = be32toh(hdr.body_len);
    std::string value;
    if (body_len > 0) {
        value.resize(body_len);
        if (!recv_all(fd, value.data(), body_len)) return std::nullopt;
    }
    
    Response resp;
    resp.status = static_cast<Status>(be16toh(hdr.vbucket));
    resp.value = std::move(value);
    resp.opaque = be32toh(hdr.opaque);
    resp.cas = be64toh(hdr.cas);
    return resp;
}

size_t ConnectionPool::idle_count() const {
    std::lock_guard lock(mu_);
    size_t total = 0;
    for (const auto& [k, q] : pool_) total += q.size();
    return total;
}

}  // namespace pensieve
