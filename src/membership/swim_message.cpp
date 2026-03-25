#include "membership/swim_message.h"

#include <cstring>

namespace pensieve {

namespace {

void write_u8(std::vector<uint8_t>& buf, uint8_t v) { buf.push_back(v); }

void write_u16(std::vector<uint8_t>& buf, uint16_t v) {
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v));
}

void write_u32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v >> 24));
    buf.push_back(static_cast<uint8_t>(v >> 16));
    buf.push_back(static_cast<uint8_t>(v >> 8));
    buf.push_back(static_cast<uint8_t>(v));
}

void write_u64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 56; i >= 0; i -= 8) {
        buf.push_back(static_cast<uint8_t>(v >> i));
    }
}

bool read_u8(std::span<const uint8_t> data, size_t& off, uint8_t& out) {
    if (off >= data.size()) return false;
    out = data[off++];
    return true;
}

bool read_u16(std::span<const uint8_t> data, size_t& off, uint16_t& out) {
    if (off + 2 > data.size()) return false;
    out = static_cast<uint16_t>((data[off] << 8) | data[off + 1]);
    off += 2;
    return true;
}

bool read_u32(std::span<const uint8_t> data, size_t& off, uint32_t& out) {
    if (off + 4 > data.size()) return false;
    out = (static_cast<uint32_t>(data[off]) << 24) |
          (static_cast<uint32_t>(data[off + 1]) << 16) |
          (static_cast<uint32_t>(data[off + 2]) << 8) |
          static_cast<uint32_t>(data[off + 3]);
    off += 4;
    return true;
}

bool read_u64(std::span<const uint8_t> data, size_t& off, uint64_t& out) {
    if (off + 8 > data.size()) return false;
    out = 0;
    for (int i = 0; i < 8; ++i) {
        out = (out << 8) | data[off + i];
    }
    off += 8;
    return true;
}

void serialize_update(std::vector<uint8_t>& buf, const MembershipUpdate& u) {
    write_u8(buf, static_cast<uint8_t>(u.type));
    serialize_node_id(buf, u.node);
    write_u64(buf, u.incarnation);
    write_u16(buf, u.data_port);
}

std::optional<MembershipUpdate> deserialize_update(std::span<const uint8_t> data,
                                                   size_t& off) {
    uint8_t type_val;
    if (!read_u8(data, off, type_val)) return std::nullopt;
    if (type_val > static_cast<uint8_t>(MembershipUpdate::Type::Leave))
        return std::nullopt;

    auto node = deserialize_node_id(data, off);
    if (!node) return std::nullopt;

    uint64_t inc;
    if (!read_u64(data, off, inc)) return std::nullopt;

    uint16_t dp = 0;
    if (!read_u16(data, off, dp)) return std::nullopt;

    return MembershipUpdate{
        static_cast<MembershipUpdate::Type>(type_val),
        std::move(*node),
        inc,
        dp};
}

}  // namespace

void serialize_node_id(std::vector<uint8_t>& buf, const NodeId& id) {
    write_u16(buf, static_cast<uint16_t>(id.host.size()));
    buf.insert(buf.end(), id.host.begin(), id.host.end());
    write_u16(buf, id.gossip_port);
}

std::optional<NodeId> deserialize_node_id(std::span<const uint8_t> data,
                                          size_t& offset) {
    uint16_t host_len;
    if (!read_u16(data, offset, host_len)) return std::nullopt;
    if (offset + host_len > data.size()) return std::nullopt;

    std::string host(data.begin() + offset, data.begin() + offset + host_len);
    offset += host_len;

    uint16_t port;
    if (!read_u16(data, offset, port)) return std::nullopt;

    return NodeId{std::move(host), port};
}

std::vector<uint8_t> SwimMessage::serialize() const {
    std::vector<uint8_t> buf;
    buf.reserve(128);

    write_u8(buf, static_cast<uint8_t>(type));
    write_u32(buf, seq_num);
    serialize_node_id(buf, sender);

    if (type == SwimMessageType::PingReq && target) {
        serialize_node_id(buf, *target);
    }

    write_u16(buf, static_cast<uint16_t>(updates.size()));
    for (const auto& u : updates) {
        serialize_update(buf, u);
    }

    return buf;
}

std::optional<SwimMessage> SwimMessage::deserialize(
    std::span<const uint8_t> data) {
    size_t off = 0;

    uint8_t type_val;
    if (!read_u8(data, off, type_val)) return std::nullopt;
    if (type_val < 1 || type_val > 3) return std::nullopt;

    uint32_t seq;
    if (!read_u32(data, off, seq)) return std::nullopt;

    auto sender = deserialize_node_id(data, off);
    if (!sender) return std::nullopt;

    SwimMessage msg;
    msg.type = static_cast<SwimMessageType>(type_val);
    msg.seq_num = seq;
    msg.sender = std::move(*sender);

    if (msg.type == SwimMessageType::PingReq) {
        auto target = deserialize_node_id(data, off);
        if (!target) return std::nullopt;
        msg.target = std::move(*target);
    }

    uint16_t update_count;
    if (!read_u16(data, off, update_count)) return std::nullopt;

    for (uint16_t i = 0; i < update_count; ++i) {
        auto u = deserialize_update(data, off);
        if (!u) return std::nullopt;
        msg.updates.push_back(std::move(*u));
    }

    return msg;
}

}  // namespace pensieve
