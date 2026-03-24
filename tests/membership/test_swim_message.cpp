#include "membership/swim_message.h"

#include <gtest/gtest.h>

using namespace pensieve;

TEST(SwimMessage, PingRoundTrip) {
    SwimMessage msg;
    msg.type = SwimMessageType::Ping;
    msg.seq_num = 42;
    msg.sender = {"10.0.0.1", 5000};

    auto bytes = msg.serialize();
    auto decoded = SwimMessage::deserialize(bytes);

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type, SwimMessageType::Ping);
    EXPECT_EQ(decoded->seq_num, 42u);
    EXPECT_EQ(decoded->sender.host, "10.0.0.1");
    EXPECT_EQ(decoded->sender.gossip_port, 5000);
    EXPECT_FALSE(decoded->target.has_value());
    EXPECT_TRUE(decoded->updates.empty());
}

TEST(SwimMessage, AckRoundTrip) {
    SwimMessage msg;
    msg.type = SwimMessageType::Ack;
    msg.seq_num = 99;
    msg.sender = {"192.168.1.1", 7000};

    auto bytes = msg.serialize();
    auto decoded = SwimMessage::deserialize(bytes);

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type, SwimMessageType::Ack);
    EXPECT_EQ(decoded->seq_num, 99u);
    EXPECT_EQ(decoded->sender, msg.sender);
}

TEST(SwimMessage, PingReqRoundTrip) {
    SwimMessage msg;
    msg.type = SwimMessageType::PingReq;
    msg.seq_num = 7;
    msg.sender = {"10.0.0.1", 5000};
    msg.target = NodeId{"10.0.0.5", 5000};

    auto bytes = msg.serialize();
    auto decoded = SwimMessage::deserialize(bytes);

    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->type, SwimMessageType::PingReq);
    EXPECT_EQ(decoded->seq_num, 7u);
    ASSERT_TRUE(decoded->target.has_value());
    EXPECT_EQ(decoded->target->host, "10.0.0.5");
    EXPECT_EQ(decoded->target->gossip_port, 5000);
}

TEST(SwimMessage, WithPiggybackedUpdates) {
    SwimMessage msg;
    msg.type = SwimMessageType::Ping;
    msg.seq_num = 1;
    msg.sender = {"10.0.0.1", 5000};
    msg.updates = {
        {MembershipUpdate::Type::Join, {"10.0.0.2", 5000}, 1},
        {MembershipUpdate::Type::Suspect, {"10.0.0.3", 5000}, 3},
        {MembershipUpdate::Type::Dead, {"10.0.0.4", 5000}, 5},
    };

    auto bytes = msg.serialize();
    auto decoded = SwimMessage::deserialize(bytes);

    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->updates.size(), 3u);

    EXPECT_EQ(decoded->updates[0].type, MembershipUpdate::Type::Join);
    EXPECT_EQ(decoded->updates[0].node.host, "10.0.0.2");
    EXPECT_EQ(decoded->updates[0].incarnation, 1u);

    EXPECT_EQ(decoded->updates[1].type, MembershipUpdate::Type::Suspect);
    EXPECT_EQ(decoded->updates[1].node.host, "10.0.0.3");
    EXPECT_EQ(decoded->updates[1].incarnation, 3u);

    EXPECT_EQ(decoded->updates[2].type, MembershipUpdate::Type::Dead);
    EXPECT_EQ(decoded->updates[2].node.host, "10.0.0.4");
    EXPECT_EQ(decoded->updates[2].incarnation, 5u);
}

TEST(SwimMessage, EmptyBufferReturnsNullopt) {
    auto decoded = SwimMessage::deserialize({});
    EXPECT_FALSE(decoded.has_value());
}

TEST(SwimMessage, TruncatedBufferReturnsNullopt) {
    SwimMessage msg;
    msg.type = SwimMessageType::Ping;
    msg.seq_num = 1;
    msg.sender = {"10.0.0.1", 5000};

    auto bytes = msg.serialize();
    // Truncate at various points
    for (size_t i = 1; i < bytes.size(); ++i) {
        auto truncated = std::span<const uint8_t>(bytes.data(), i);
        auto decoded = SwimMessage::deserialize(truncated);
        EXPECT_FALSE(decoded.has_value()) << "Unexpectedly decoded at size " << i;
    }
}

TEST(SwimMessage, InvalidMessageType) {
    std::vector<uint8_t> buf = {0x00, 0, 0, 0, 0};
    auto decoded = SwimMessage::deserialize(buf);
    EXPECT_FALSE(decoded.has_value());

    buf[0] = 0x04;
    decoded = SwimMessage::deserialize(buf);
    EXPECT_FALSE(decoded.has_value());
}

TEST(SwimMessage, NodeIdSerializationRoundTrip) {
    NodeId id{"some-host.example.com", 12345};
    std::vector<uint8_t> buf;
    serialize_node_id(buf, id);

    size_t offset = 0;
    auto decoded = deserialize_node_id(buf, offset);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, id);
    EXPECT_EQ(offset, buf.size());
}

TEST(SwimMessage, LargeSeqNum) {
    SwimMessage msg;
    msg.type = SwimMessageType::Ack;
    msg.seq_num = 0xDEADBEEF;
    msg.sender = {"10.0.0.1", 5000};

    auto bytes = msg.serialize();
    auto decoded = SwimMessage::deserialize(bytes);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->seq_num, 0xDEADBEEFu);
}

TEST(SwimMessage, AllUpdateTypes) {
    SwimMessage msg;
    msg.type = SwimMessageType::Ping;
    msg.seq_num = 0;
    msg.sender = {"10.0.0.1", 5000};
    msg.updates = {
        {MembershipUpdate::Type::Join, {"a", 1}, 0},
        {MembershipUpdate::Type::Leave, {"b", 2}, 1},
        {MembershipUpdate::Type::Alive, {"c", 3}, 2},
        {MembershipUpdate::Type::Suspect, {"d", 4}, 3},
        {MembershipUpdate::Type::Dead, {"e", 5}, 4},
    };

    auto bytes = msg.serialize();
    auto decoded = SwimMessage::deserialize(bytes);
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->updates.size(), 5u);

    for (size_t i = 0; i < 5; ++i) {
        EXPECT_EQ(static_cast<uint8_t>(decoded->updates[i].type),
                  static_cast<uint8_t>(i));
    }
}
