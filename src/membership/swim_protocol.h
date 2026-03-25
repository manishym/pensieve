#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <unordered_map>

#include "io/awaitable.h"
#include "io/task.h"
#include "io/udp_socket.h"
#include "membership/disseminator.h"
#include "membership/member_list.h"
#include "membership/swim_message.h"

namespace pensieve {

class SwimProtocol {
public:
    struct Config {
        std::chrono::milliseconds protocol_period = std::chrono::milliseconds{1000};
        std::chrono::milliseconds ping_timeout = std::chrono::milliseconds{200};
        std::chrono::milliseconds suspect_timeout = std::chrono::milliseconds{5000};
        uint32_t indirect_ping_peers = 3;
        size_t max_piggyback_updates = 8;
        uint16_t self_data_port = 0;
    };

    SwimProtocol(IoUringContext& ctx, UdpSocket& sock, MemberList& members,
                 Disseminator& disseminator, NodeId self, Config config);

    void run();
    void stop();

    uint64_t incarnation() const { return incarnation_; }
    uint32_t next_seq() { return seq_counter_++; }

private:
    Task<> probe_loop();
    Task<> recv_loop();
    Task<> suspicion_reaper();

    Task<bool> direct_ping(const NodeId& target);
    Task<bool> indirect_ping(const NodeId& target);

    void handle_message(const SwimMessage& msg, const sockaddr_in& from);
    void on_ping(const SwimMessage& msg, const sockaddr_in& from);
    void on_ack(const SwimMessage& msg);
    void on_ping_req(const SwimMessage& msg, const sockaddr_in& from);
    Task<> relay_ping(NodeId target, NodeId requester, uint32_t orig_seq);
    void apply_updates(const std::vector<MembershipUpdate>& updates);

    Task<> send_message(const SwimMessage& msg, const NodeId& dest);
    sockaddr_in resolve(const NodeId& id);

    IoUringContext& ctx_;
    UdpSocket& sock_;
    MemberList& members_;
    Disseminator& disseminator_;
    NodeId self_;
    Config config_;

    std::atomic<bool> running_{false};
    uint64_t incarnation_ = 0;
    uint32_t seq_counter_ = 0;

    // Pending ack tracking: seq_num -> received flag
    std::unordered_map<uint32_t, bool> pending_acks_;

    void spawn(Task<> task);

    // Owned sub-tasks (must outlive pending io_uring operations)
    Task<> probe_task_;
    Task<> recv_task_;
    Task<> reaper_task_;
    std::vector<Task<>> detached_tasks_;
};

}  // namespace pensieve
