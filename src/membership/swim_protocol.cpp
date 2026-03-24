#include "membership/swim_protocol.h"

#include <arpa/inet.h>
#include <cstring>

namespace pensieve {

SwimProtocol::SwimProtocol(IoUringContext& ctx, UdpSocket& sock,
                           MemberList& members, Disseminator& disseminator,
                           NodeId self, Config config)
    : ctx_(ctx),
      sock_(sock),
      members_(members),
      disseminator_(disseminator),
      self_(std::move(self)),
      config_(config) {}

void SwimProtocol::run() {
    running_.store(true, std::memory_order_relaxed);

    probe_task_ = probe_loop();
    recv_task_ = recv_loop();
    reaper_task_ = suspicion_reaper();

    probe_task_.start();
    recv_task_.start();
    reaper_task_.start();
}

void SwimProtocol::stop() {
    running_.store(false, std::memory_order_relaxed);
}

Task<> SwimProtocol::probe_loop() {
    while (running_.load(std::memory_order_relaxed)) {
        co_await async_timeout(ctx_, config_.protocol_period);

        if (!running_.load(std::memory_order_relaxed)) break;

        auto target = members_.random_peer(self_);
        if (!target) continue;

        bool alive = co_await direct_ping(*target);
        if (alive) continue;

        alive = co_await indirect_ping(*target);
        if (alive) continue;

        auto node_info = members_.get_node(*target);
        if (node_info &&
            members_.update_state(*target, NodeState::Suspect,
                                  node_info->incarnation)) {
            disseminator_.enqueue({MembershipUpdate::Type::Suspect, *target,
                                   node_info->incarnation});
        }
    }
}

Task<> SwimProtocol::recv_loop() {
    char buf[65536];
    sockaddr_in from{};

    while (running_.load(std::memory_order_relaxed)) {
        int32_t n = co_await async_recvfrom(ctx_, sock_.fd(), buf, sizeof(buf),
                                            &from);
        if (n <= 0) continue;

        auto msg = SwimMessage::deserialize(
            std::span<const uint8_t>(reinterpret_cast<uint8_t*>(buf), n));
        if (!msg) continue;

        handle_message(*msg, from);
    }
}

Task<> SwimProtocol::suspicion_reaper() {
    while (running_.load(std::memory_order_relaxed)) {
        co_await async_timeout(ctx_, config_.suspect_timeout / 2);
        if (!running_.load(std::memory_order_relaxed)) break;

        auto now = std::chrono::steady_clock::now();
        for (const auto& [id, node] : members_.all_nodes()) {
            if (id == self_) continue;
            if (node.state != NodeState::Suspect) continue;

            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - node.last_state_change);
            if (elapsed >= config_.suspect_timeout) {
                if (members_.update_state(id, NodeState::Dead,
                                          node.incarnation)) {
                    disseminator_.enqueue({MembershipUpdate::Type::Dead,
                                           id, node.incarnation});
                }
            }
        }
    }
}

Task<bool> SwimProtocol::direct_ping(const NodeId& target) {
    uint32_t seq = next_seq();
    pending_acks_[seq] = false;

    SwimMessage ping;
    ping.type = SwimMessageType::Ping;
    ping.seq_num = seq;
    ping.sender = self_;
    ping.updates = disseminator_.get_updates(config_.max_piggyback_updates);

    co_await send_message(ping, target);
    co_await async_timeout(ctx_, config_.ping_timeout);

    bool acked = pending_acks_[seq];
    pending_acks_.erase(seq);
    co_return acked;
}

Task<bool> SwimProtocol::indirect_ping(const NodeId& target) {
    auto intermediaries = members_.random_peers(config_.indirect_ping_peers,
                                                self_);
    if (intermediaries.empty()) co_return false;

    uint32_t seq = next_seq();
    pending_acks_[seq] = false;

    for (const auto& relay : intermediaries) {
        SwimMessage ping_req;
        ping_req.type = SwimMessageType::PingReq;
        ping_req.seq_num = seq;
        ping_req.sender = self_;
        ping_req.target = target;
        ping_req.updates =
            disseminator_.get_updates(config_.max_piggyback_updates);

        co_await send_message(ping_req, relay);
    }

    co_await async_timeout(ctx_, config_.ping_timeout);

    bool acked = pending_acks_[seq];
    pending_acks_.erase(seq);
    co_return acked;
}

void SwimProtocol::handle_message(const SwimMessage& msg,
                                  const sockaddr_in& from) {
    apply_updates(msg.updates);

    switch (msg.type) {
    case SwimMessageType::Ping:
        on_ping(msg, from);
        break;
    case SwimMessageType::Ack:
        on_ack(msg);
        break;
    case SwimMessageType::PingReq:
        on_ping_req(msg, from);
        break;
    }
}

void SwimProtocol::on_ping(const SwimMessage& msg, const sockaddr_in& from) {
    (void)from;
    SwimMessage ack;
    ack.type = SwimMessageType::Ack;
    ack.seq_num = msg.seq_num;
    ack.sender = self_;
    ack.updates = disseminator_.get_updates(config_.max_piggyback_updates);

    spawn(send_message(ack, msg.sender));
}

void SwimProtocol::on_ack(const SwimMessage& msg) {
    auto it = pending_acks_.find(msg.seq_num);
    if (it != pending_acks_.end()) {
        it->second = true;
    }

    if (auto node = members_.get_node(msg.sender)) {
        if (node->state == NodeState::Suspect || node->state == NodeState::Dead) {
            members_.update_state(msg.sender, NodeState::Alive,
                                  node->incarnation);
        }
    }
}

void SwimProtocol::on_ping_req(const SwimMessage& msg,
                               const sockaddr_in& from) {
    (void)from;
    if (!msg.target) return;
    spawn(relay_ping(*msg.target, msg.sender, msg.seq_num));
}

// cppcheck-suppress passedByValue ; coroutine must own copies that outlive caller
Task<> SwimProtocol::relay_ping(NodeId target, NodeId requester,
                                uint32_t orig_seq) {
    SwimMessage ping;
    ping.type = SwimMessageType::Ping;
    ping.seq_num = next_seq();
    ping.sender = self_;

    uint32_t local_seq = ping.seq_num;
    pending_acks_[local_seq] = false;

    co_await send_message(ping, target);
    co_await async_timeout(ctx_, config_.ping_timeout);

    if (pending_acks_[local_seq]) {
        SwimMessage ack;
        ack.type = SwimMessageType::Ack;
        ack.seq_num = orig_seq;
        ack.sender = self_;
        co_await send_message(ack, requester);
    }
    pending_acks_.erase(local_seq);
}

void SwimProtocol::apply_updates(
    const std::vector<MembershipUpdate>& updates) {
    for (const auto& u : updates) {
        if (u.node == self_) {
            if (u.type == MembershipUpdate::Type::Suspect ||
                u.type == MembershipUpdate::Type::Dead) {
                ++incarnation_;
                disseminator_.enqueue({MembershipUpdate::Type::Alive, self_,
                                       incarnation_});
                members_.update_state(self_, NodeState::Alive, incarnation_);
            }
            continue;
        }

        NodeState new_state;
        switch (u.type) {
        case MembershipUpdate::Type::Join:
        case MembershipUpdate::Type::Alive:
            new_state = NodeState::Alive;
            break;
        case MembershipUpdate::Type::Suspect:
            new_state = NodeState::Suspect;
            break;
        case MembershipUpdate::Type::Dead:
            new_state = NodeState::Dead;
            break;
        case MembershipUpdate::Type::Leave:
            new_state = NodeState::Left;
            break;
        }

        if (u.type == MembershipUpdate::Type::Join) {
            NodeInfo info;
            info.id = u.node;
            info.state = NodeState::Alive;
            info.incarnation = u.incarnation;
            members_.add_node(info);
        } else {
            members_.update_state(u.node, new_state, u.incarnation);
        }
    }
}

Task<> SwimProtocol::send_message(const SwimMessage& msg,
                                  const NodeId& dest) {
    auto bytes = msg.serialize();
    auto addr = resolve(dest);
    co_await async_sendto(ctx_, sock_.fd(), bytes.data(), bytes.size(), addr);
}

sockaddr_in SwimProtocol::resolve(const NodeId& id) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(id.gossip_port);
    inet_pton(AF_INET, id.host.c_str(), &addr.sin_addr);
    return addr;
}

void SwimProtocol::spawn(Task<> task) {
    task.start();
    detached_tasks_.push_back(std::move(task));
    std::erase_if(detached_tasks_,
                  [](const Task<>& t) { return t.done(); });
}

}  // namespace pensieve
