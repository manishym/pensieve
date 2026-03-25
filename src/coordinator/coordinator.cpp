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
        RequestHeader hdr{};
        if (!co_await read_exact(ctx_, client_fd, &hdr, sizeof(hdr))) break;

        size_t payload_len =
            static_cast<size_t>(hdr.key_len) + hdr.value_len;
        std::vector<uint8_t> payload(payload_len);
        if (payload_len > 0 &&
            !co_await read_exact(ctx_, client_fd, payload.data(),
                                 payload_len)) {
            break;
        }

        Request req;
        req.opcode = hdr.opcode;
        req.key.assign(reinterpret_cast<const char*>(payload.data()),
                       hdr.key_len);
        if (hdr.value_len > 0) {
            req.value.assign(
                reinterpret_cast<const char*>(payload.data()) + hdr.key_len,
                hdr.value_len);
        }

        if (req.opcode == Opcode::ClusterInfo) {
            auto resp = serve_cluster_info();
            if (!co_await write_response(client_fd, resp)) break;
            continue;
        }

        uint32_t hash = hash_key(req.key);
        auto owner = ring_.get_node_for_key(hash);

        Response resp;
        if (!owner.has_value() || *owner == self_) {
            resp = co_await serve_local(req);
        } else {
            if (req.opcode == Opcode::Get) {
                auto [is_initiator, handle] =
                    wait_group_.try_join(req.key);
                if (is_initiator) {
                    resp = co_await proxy_to_peer(req, *owner);
                    wait_group_.complete(
                        req.key,
                        resp.status == Status::Ok
                            ? std::optional(resp.value)
                            : std::nullopt);
                } else {
                    auto result =
                        co_await wait_group_.wait(std::move(handle));
                    if (result.has_value()) {
                        resp = {Status::Ok, std::move(*result)};
                    } else {
                        resp = {Status::NotFound, {}};
                    }
                }
            } else {
                resp = co_await proxy_to_peer(req, *owner);
            }
        }

        if (!co_await write_response(client_fd, resp)) break;
    }
    co_await async_close(ctx_, client_fd);
}

Task<Response> Coordinator::serve_local(const Request& req) {
    switch (req.opcode) {
        case Opcode::Get: {
            auto val = store_.get(req.key);
            if (val.has_value()) {
                co_return Response{Status::Ok, std::move(*val)};
            }
            co_return Response{Status::NotFound, {}};
        }
        case Opcode::Put: {
            bool ok = store_.put(req.key, req.value);
            co_return Response{ok ? Status::Ok : Status::Error, {}};
        }
        case Opcode::Del: {
            bool ok = store_.del(req.key);
            co_return Response{ok ? Status::Ok : Status::NotFound, {}};
        }
    }
    co_return Response{Status::Error, {}};
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
    ResponseHeader hdr{};
    hdr.status    = resp.status;
    hdr.flags     = 0;
    hdr.reserved  = 0;
    hdr.value_len = static_cast<uint32_t>(resp.value.size());

    if (!co_await write_all(ctx_, client_fd, &hdr, sizeof(hdr)))
        co_return false;

    if (resp.value.empty()) co_return true;

    if (pool_) {
        auto handle = pool_->acquire();
        if (handle && handle->size >= resp.value.size()) {
            std::memcpy(handle->data, resp.value.data(), resp.value.size());
            int32_t n = co_await async_write_fixed(
                ctx_, client_fd, handle->data, resp.value.size(),
                handle->index);
            pool_->release(handle->index);
            co_return n > 0;
        }
        if (handle) pool_->release(handle->index);
    }

    co_return co_await write_all(ctx_, client_fd, resp.value.data(),
                                  resp.value.size());
}

void Coordinator::spawn(Task<> task) {
    task.start();
    detached_.push_back(std::move(task));
    std::erase_if(detached_, [](const Task<>& t) { return t.done(); });
}

}  // namespace pensieve
