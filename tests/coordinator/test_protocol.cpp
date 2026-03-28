#include "protocol/message.h"

#include <gtest/gtest.h>

using namespace pensieve;

TEST(Protocol, MemHeaderSize) {
    EXPECT_EQ(sizeof(MemHeader), 24u);
}

TEST(Protocol, GetRequestRoundTrip) {
    Request req{Opcode::Get, "mykey", ""};
    auto wire = serialize_request(req);

    EXPECT_EQ(wire.size(), sizeof(MemHeader) + 5);

    auto parsed = parse_request(wire.data(), wire.size());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->opcode, Opcode::Get);
    EXPECT_EQ(parsed->key, "mykey");
    EXPECT_TRUE(parsed->value.empty());
}

TEST(Protocol, PutRequestRoundTrip) {
    Request req{Opcode::Put, "key1", "value123"};
    auto wire = serialize_request(req);

    EXPECT_EQ(wire.size(), sizeof(MemHeader) + 4 + 8);

    auto parsed = parse_request(wire.data(), wire.size());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->opcode, Opcode::Put);
    EXPECT_EQ(parsed->key, "key1");
    EXPECT_EQ(parsed->value, "value123");
}

TEST(Protocol, DelRequestRoundTrip) {
    Request req{Opcode::Del, "to_delete", ""};
    auto wire = serialize_request(req);
    auto parsed = parse_request(wire.data(), wire.size());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->opcode, Opcode::Del);
    EXPECT_EQ(parsed->key, "to_delete");
}

TEST(Protocol, OkResponseRoundTrip) {
    Response resp{Status::Ok, "hello world"};
    auto wire = serialize_response(resp);

    EXPECT_EQ(wire.size(), sizeof(MemHeader) + 11);

    auto parsed = parse_response(wire.data(), wire.size());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->status, Status::Ok);
    EXPECT_EQ(parsed->value, "hello world");
}

TEST(Protocol, NotFoundResponseRoundTrip) {
    Response resp{Status::NotFound, ""};
    auto wire = serialize_response(resp);

    auto parsed = parse_response(wire.data(), wire.size());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->status, Status::NotFound);
    EXPECT_TRUE(parsed->value.empty());
}

TEST(Protocol, ErrorResponseRoundTrip) {
    Response resp{Status::Error, ""};
    auto wire = serialize_response(resp);
    auto parsed = parse_response(wire.data(), wire.size());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->status, Status::Error);
}

TEST(Protocol, TruncatedRequestHeaderFails) {
    uint8_t buf[20] = {}; // Less than 24
    EXPECT_FALSE(parse_request(buf, 20).has_value());
}

TEST(Protocol, TruncatedRequestPayloadFails) {
    Request req{Opcode::Get, "longkey", ""};
    auto wire = serialize_request(req);
    EXPECT_FALSE(parse_request(wire.data(), wire.size() - 1).has_value());
}

TEST(Protocol, TruncatedResponseHeaderFails) {
    uint8_t buf[20] = {};
    EXPECT_FALSE(parse_response(buf, 20).has_value());
}

TEST(Protocol, TruncatedResponsePayloadFails) {
    Response resp{Status::Ok, "data"};
    auto wire = serialize_response(resp);
    EXPECT_FALSE(parse_response(wire.data(), wire.size() - 1).has_value());
}

TEST(Protocol, EmptyKeyRequest) {
    Request req{Opcode::Get, "", ""};
    auto wire = serialize_request(req);
    auto parsed = parse_request(wire.data(), wire.size());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->key.empty());
}

TEST(Protocol, LargeValue) {
    std::string big_value(65535, 'X');
    Request req{Opcode::Put, "k", big_value};
    auto wire = serialize_request(req);
    auto parsed = parse_request(wire.data(), wire.size());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->value, big_value);
}
