#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "membership/node_info.h"

namespace pensieve {

// Wire format:
//   MsgType(1) | SeqNum(4) | SenderID(var) | payload(var) | updates(var)
//
// NodeId encoding: host_len(2) + host_bytes + port(2)
// MembershipUpdate: type(1) + NodeId(var) + incarnation(8)
// Piggyback: update_count(2) + N * MembershipUpdate

enum class SwimMessageType : uint8_t { Ping = 1, Ack = 2, PingReq = 3 };

struct MembershipUpdate {
    enum class Type : uint8_t { Join = 0, Alive = 1, Suspect = 2, Dead = 3, Leave = 4 };
    Type type;
    NodeId node;
    uint64_t incarnation = 0;
    uint16_t data_port = 0;
};

struct SwimMessage {
    SwimMessageType type;
    uint32_t seq_num = 0;
    NodeId sender;
    std::optional<NodeId> target;  // only for PingReq
    std::vector<MembershipUpdate> updates;

    std::vector<uint8_t> serialize() const;
    static std::optional<SwimMessage> deserialize(std::span<const uint8_t> data);
};

// Low-level serialization helpers (public for testing)
void serialize_node_id(std::vector<uint8_t>& buf, const NodeId& id);
std::optional<NodeId> deserialize_node_id(std::span<const uint8_t> data,
                                          size_t& offset);

}  // namespace pensieve
