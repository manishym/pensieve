#include "coordinator/coordinator.h"

#include <cstring>

#include "hash/hasher.h"
#include "io/stream_utils.h"

namespace pensieve {

Coordinator::Coordinator(IoUringContext& ctx, LocalStore& store,
                         RingStore& ring, MemberList& members,
                         NodeId self, BufferPool* pool)
    : ctx_(ctx),
      store_(store),
      ring_(ring),
      members_(members),
      self_(std::move(self)),
      pool_(pool),
      peer_client_(ctx, pool) {}

Task<> Coordinator::handle_connection(fd_t client_fd) {
    while (true) {
        MemHeader hdr{};
        if (!co_await read_exact(ctx_, client_fd, &hdr, sizeof(hdr))) break;

        if (hdr.magic != 0x80) break; // Must be Request magic

        uint16_t key_len = be16toh(hdr.key_len);
        uint32_t body_len = be32toh(hdr.body_len);
        uint8_t ext_len = hdr.ext_len;
        uint32_t opaque = be32toh(hdr.opaque);
        uint64_t cas = be64toh(hdr.cas);

        if (ext_len + key_len > body_len) break;

        struct HandleGuard {
            BufferPool* pool;
            std::optional<BufferPool::BufferHandle> handle;
            ~HandleGuard() { if (pool && handle) pool->release(handle->index); }
        };
        HandleGuard guard{pool_, std::nullopt};
        uint8_t* payload_data = nullptr;
        std::vector<uint8_t> fallback;

        if (body_len > 0) {
            if (pool_ && body_len <= pool_->buffer_size()) {
                guard.handle = pool_->acquire();
            }
            if (guard.handle) {
                payload_data = guard.handle->data;
            } else {
                fallback.resize(body_len);
                payload_data = fallback.data();
            }

            if (!co_await read_exact(ctx_, client_fd, payload_data, body_len)) {
                break;
            }
        }

        Request req;
        req.opcode = static_cast<Opcode>(hdr.opcode);
        req.opaque = opaque;
        req.cas = cas;

        if (key_len > 0 && payload_data) {
            req.key = std::string_view(reinterpret_cast<const char*>(payload_data) + ext_len, key_len);
        }
        
        uint32_t value_len = body_len - ext_len - key_len;
        if (value_len > 0 && payload_data) {
            req.value = std::string_view(reinterpret_cast<const char*>(payload_data) + ext_len + key_len, value_len);
        }

        Response resp = co_await route_request(req);

        resp.opaque = req.opaque;
        resp.cas = req.cas;

        bool ok = co_await write_response(client_fd, resp);
        if (!ok) break;
    }
    co_await async_close(ctx_, client_fd);
}

Task<Response> Coordinator::serve_local(const Request& req) {
    switch (req.opcode) {
        case Opcode::Get: {
            auto val = store_.get(req.key);
            if (val.has_value()) {
                co_return Response{Status::Ok, std::move(*val), req.opaque, req.cas};
            }
            co_return Response{Status::NotFound, {}, req.opaque, req.cas};
        }
        case Opcode::Set: {
            bool ok = store_.put(req.key, req.value);
            co_return Response{ok ? Status::Ok : Status::Error, {}, req.opaque, req.cas}; // Spec: 0x0008 OutOfMemory 
        }
        case Opcode::Del: {
            bool ok = store_.del(req.key);
            co_return Response{ok ? Status::Ok : Status::NotFound, {}, req.opaque, req.cas};
        }
        case Opcode::ClusterInfo: {
            break;
        }
    }
    co_return Response{Status::Error, {}, req.opaque, req.cas};
}

Task<Response> Coordinator::route_request(const Request& req) {
    if (req.opcode == Opcode::ClusterInfo) {
        co_return serve_cluster_info();
    }

    uint32_t hash = hash_key(req.key);
    auto owner = ring_.get_node_for_key(hash);

    if (!owner.has_value() || *owner == self_) {
        co_return co_await serve_local(req);
    }

    if (req.opcode == Opcode::Get) {
        auto [is_initiator, w_handle] = wait_group_.try_join(req.key);
        if (is_initiator) {
            auto resp = co_await proxy_to_peer(req, *owner);
            wait_group_.complete(
                req.key,
                resp.status == Status::Ok ? std::optional(resp.value) : std::nullopt);
            co_return resp;
        } else {
            auto result = co_await wait_group_.wait(std::move(w_handle));
            if (result.has_value()) {
                co_return Response{Status::Ok, std::move(*result), req.opaque, req.cas};
            } else {
                co_return Response{Status::NotFound, {}, req.opaque, req.cas};
            }
        }
    }

    co_return co_await proxy_to_peer(req, *owner);
}

Response Coordinator::serve_cluster_info() {
    const auto& nodes = members_.all_nodes();

    std::string buf;
    uint16_t count = 0;
    for (const auto& [id, info] : nodes) {
        if (info.state == NodeState::Alive) ++count;
    }

    buf.resize(sizeof(uint16_t));
    std::memcpy(buf.data(), &count, sizeof(count));

    for (const auto& [id, info] : nodes) {
        if (info.state != NodeState::Alive) continue;

        uint16_t host_len = static_cast<uint16_t>(id.host.size());
        size_t off = buf.size();
        buf.resize(off + sizeof(host_len) + host_len +
                   sizeof(id.gossip_port) + sizeof(info.data_port));

        auto* p = buf.data() + off;
        std::memcpy(p, &host_len, sizeof(host_len));
        p += sizeof(host_len);
        std::memcpy(p, id.host.data(), host_len);
        p += host_len;
        std::memcpy(p, &id.gossip_port, sizeof(id.gossip_port));
        p += sizeof(id.gossip_port);
        std::memcpy(p, &info.data_port, sizeof(info.data_port));
    }

    return {Status::Ok, std::move(buf)};
}

Task<Response> Coordinator::proxy_to_peer(const Request& req,
                                          const NodeId& owner) {
    auto info = members_.get_node(owner);
    if (!info.has_value()) {
        co_return Response{Status::Error, {}};
    }

    fd_t peer_fd = co_await peer_client_.acquire(owner.host,
                                                  info->data_port);
    if (peer_fd < 0) {
        co_return Response{Status::Error, {}};
    }

    Response resp = co_await peer_client_.send_request(peer_fd, req);
    if (resp.status == Status::Error) {
        co_await async_close(ctx_, peer_fd);
    } else {
        peer_client_.release(owner.host, info->data_port, peer_fd);
    }
    co_return resp;
}

Task<bool> Coordinator::write_response(fd_t client_fd, const Response& resp) {
    MemHeader hdr{};
    hdr.magic = 0x81;
    hdr.opcode = 0x00;
    hdr.key_len = 0;
    hdr.ext_len = 0;
    hdr.data_type = 0;
    hdr.vbucket = htobe16(static_cast<uint16_t>(resp.status));
    hdr.body_len = htobe32(static_cast<uint32_t>(resp.value.size()));
    hdr.opaque = htobe32(resp.opaque);
    hdr.cas = htobe64(resp.cas);

    if (resp.value.empty()) {
        co_return co_await write_all(ctx_, client_fd, &hdr, sizeof(hdr));
    }

    std::vector<uint8_t> buf(sizeof(hdr) + resp.value.size());
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), resp.value.data(),
                resp.value.size());
    co_return co_await write_all(ctx_, client_fd, buf.data(), buf.size());
}

void Coordinator::spawn(Task<> task) {
    task.start();
    detached_.push_back(std::move(task));
    std::erase_if(detached_, [](const Task<>& t) { return t.done(); });
}

}  // namespace pensieve
