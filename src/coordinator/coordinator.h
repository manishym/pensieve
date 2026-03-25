#pragma once

#include <vector>

#include "common/types.h"
#include "coordinator/peer_client.h"
#include "coordinator/wait_group.h"
#include "io/buffer_pool.h"
#include "io/task.h"
#include "membership/member_list.h"
#include "membership/node_info.h"
#include "membership/ring_store.h"
#include "protocol/message.h"
#include "storage/local_store.h"

namespace pensieve {

// The Coordinator routes each incoming cache request to the correct node.
// If the hashed key maps to this node, the request is served from LocalStore.
// If the key maps to a remote peer, the coordinator proxies the request
// through a pooled outbound TCP connection, optionally using registered
// buffers for zero-copy data transfer.
class Coordinator {
public:
    Coordinator(IoUringContext& ctx, LocalStore& store, RingStore& ring,
                MemberList& members, NodeId self,
                BufferPool* pool = nullptr);

    Task<> handle_connection(fd_t client_fd);

    WaitGroup& wait_group() { return wait_group_; }

private:
    Task<Response> serve_local(const Request& req);
    Task<Response> proxy_to_peer(const Request& req, const NodeId& owner);
    Response serve_cluster_info();
    Task<bool> write_response(fd_t client_fd, const Response& resp);

    IoUringContext& ctx_;
    LocalStore& store_;
    RingStore& ring_;
    MemberList& members_;
    NodeId self_;
    BufferPool* pool_;
    PeerClient peer_client_;
    WaitGroup wait_group_;

    void spawn(Task<> task);
    std::vector<Task<>> detached_;
};

}  // namespace pensieve
