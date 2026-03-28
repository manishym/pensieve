#include "sdk/topology_manager.h"

#include <arpa/inet.h>
#include <cstring>
#include <netdb.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include "hash/hasher.h"
#include "protocol/message.h"

namespace pensieve {

namespace {

constexpr uint32_t kVnodesPerNode = 128;

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

TopologyManager::~TopologyManager() {
    stop_background_refresh();
}

bool TopologyManager::fetch_topology(const std::string& host, uint16_t port,
                                     std::vector<NodeEndpoint>& out) {
    int fd = tcp_connect(host, port);
    if (fd < 0) return false;

    MemHeader hdr{};
    hdr.magic = 0x80;
    hdr.opcode = static_cast<uint8_t>(Opcode::ClusterInfo);
    hdr.key_len = 0;
    hdr.ext_len = 0;
    hdr.data_type = 0;
    hdr.vbucket = 0;
    hdr.body_len = 0;
    hdr.opaque = 0;
    hdr.cas = 0;

    bool ok = send_all(fd, &hdr, sizeof(hdr));
    if (!ok) { close(fd); return false; }

    MemHeader resp_hdr{};
    ok = recv_all(fd, &resp_hdr, sizeof(resp_hdr));
    if (!ok || resp_hdr.magic != 0x81) { close(fd); return false; }

    Status status = static_cast<Status>(be16toh(resp_hdr.vbucket));
    if (status != Status::Ok) { close(fd); return false; }

    uint32_t body_len = be32toh(resp_hdr.body_len);
    std::string payload(body_len, '\0');
    if (body_len > 0) {
        ok = recv_all(fd, payload.data(), body_len);
        if (!ok) { close(fd); return false; }
    }
    close(fd);

    if (payload.size() < sizeof(uint16_t)) return false;

    const uint8_t* p = reinterpret_cast<const uint8_t*>(payload.data());
    const uint8_t* end = p + payload.size();

    uint16_t num_nodes;
    std::memcpy(&num_nodes, p, sizeof(num_nodes));
    p += sizeof(num_nodes);

    out.clear();
    out.reserve(num_nodes);
    for (uint16_t i = 0; i < num_nodes; ++i) {
        if (p + sizeof(uint16_t) > end) return false;
        uint16_t host_len;
        std::memcpy(&host_len, p, sizeof(host_len));
        p += sizeof(host_len);

        if (p + host_len + 2 * sizeof(uint16_t) > end) return false;
        std::string node_host(reinterpret_cast<const char*>(p), host_len);
        p += host_len;

        uint16_t gossip_port, data_port;
        std::memcpy(&gossip_port, p, sizeof(gossip_port));
        p += sizeof(gossip_port);
        std::memcpy(&data_port, p, sizeof(data_port));
        p += sizeof(data_port);

        out.push_back({std::move(node_host), gossip_port, data_port});
    }
    return true;
}

bool TopologyManager::bootstrap(const std::string& seed_host,
                                uint16_t seed_port) {
    std::vector<NodeEndpoint> nodes;
    if (!fetch_topology(seed_host, seed_port, nodes)) return false;
    if (nodes.empty()) return false;

    std::lock_guard lock(mu_);
    nodes_ = std::move(nodes);
    rebuild_ring();
    return true;
}

bool TopologyManager::refresh() {
    std::vector<NodeEndpoint> current;
    {
        std::lock_guard lock(mu_);
        current = nodes_;
    }

    for (const auto& ep : current) {
        std::vector<NodeEndpoint> fresh;
        if (fetch_topology(ep.host, ep.data_port, fresh) && !fresh.empty()) {
            std::lock_guard lock(mu_);
            nodes_ = std::move(fresh);
            rebuild_ring();
            return true;
        }
    }
    return false;
}

void TopologyManager::start_background_refresh(std::chrono::seconds interval) {
    if (interval.count() <= 0) return;
    if (running_.exchange(true)) return;
    refresh_thread_ = std::thread(&TopologyManager::refresh_loop, this,
                                  interval);
}

void TopologyManager::stop_background_refresh() {
    running_.store(false);
    if (refresh_thread_.joinable()) refresh_thread_.join();
}

std::optional<NodeEndpoint> TopologyManager::find_owner(
    std::string_view key) const {
    uint32_t hash = hash_key(key);
    std::lock_guard lock(mu_);
    if (ring_.empty()) return std::nullopt;

    auto it = ring_.lower_bound(hash);
    if (it == ring_.end()) it = ring_.begin();
    return nodes_[it->second];
}

std::vector<NodeEndpoint> TopologyManager::find_successors(
    std::string_view key, size_t n) const {
    uint32_t hash = hash_key(key);
    std::lock_guard lock(mu_);
    if (ring_.empty()) return {};

    std::vector<NodeEndpoint> result;
    result.reserve(n);
    std::vector<size_t> seen;

    auto it = ring_.lower_bound(hash);
    if (it == ring_.end()) it = ring_.begin();
    auto start = it;

    do {
        size_t idx = it->second;
        if (std::find(seen.begin(), seen.end(), idx) == seen.end()) {
            seen.push_back(idx);
            result.push_back(nodes_[idx]);
            if (result.size() == n) break;
        }
        ++it;
        if (it == ring_.end()) it = ring_.begin();
    } while (it != start);

    return result;
}

std::vector<NodeEndpoint> TopologyManager::all_endpoints() const {
    std::lock_guard lock(mu_);
    return nodes_;
}

size_t TopologyManager::node_count() const {
    std::lock_guard lock(mu_);
    return nodes_.size();
}

void TopologyManager::rebuild_ring() {
    ring_.clear();
    for (size_t i = 0; i < nodes_.size(); ++i) {
        NodeId nid{nodes_[i].host, nodes_[i].gossip_port};
        for (uint32_t v = 0; v < kVnodesPerNode; ++v) {
            ring_[hash_node_token(nid, v)] = i;
        }
    }
}

void TopologyManager::refresh_loop(std::chrono::seconds interval) {
    while (running_.load()) {
        for (int i = 0; i < interval.count() && running_.load(); ++i) {
            std::this_thread::sleep_for(std::chrono::seconds{1});
        }
        if (running_.load()) refresh();
    }
}

}  // namespace pensieve
