#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pensieve {

enum class Opcode : uint8_t { Get = 1, Put = 2, Del = 3 };
enum class Status : uint8_t { Ok = 0, NotFound = 1, Error = 2 };

// On-wire layout: 8 bytes, little-endian, no padding.
struct RequestHeader {
    Opcode   opcode;
    uint8_t  flags;
    uint16_t key_len;
    uint32_t value_len;
};
static_assert(sizeof(RequestHeader) == 8);

struct ResponseHeader {
    Status   status;
    uint8_t  flags;
    uint16_t reserved;
    uint32_t value_len;
};
static_assert(sizeof(ResponseHeader) == 8);

// Parsed request with owned key/value data.
struct Request {
    Opcode opcode;
    std::string key;
    std::string value;
};

// Parsed response with owned value data.
struct Response {
    Status status;
    std::string value;
};

// ---------------------------------------------------------------------------
// Serialization helpers (write into byte vectors)
// ---------------------------------------------------------------------------

inline std::vector<uint8_t> serialize_request(const Request& req) {
    RequestHeader hdr{};
    hdr.opcode    = req.opcode;
    hdr.flags     = 0;
    hdr.key_len   = static_cast<uint16_t>(req.key.size());
    hdr.value_len = static_cast<uint32_t>(req.value.size());

    std::vector<uint8_t> buf(sizeof(hdr) + req.key.size() + req.value.size());
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), req.key.data(), req.key.size());
    std::memcpy(buf.data() + sizeof(hdr) + req.key.size(),
                req.value.data(), req.value.size());
    return buf;
}

inline std::vector<uint8_t> serialize_response(const Response& resp) {
    ResponseHeader hdr{};
    hdr.status    = resp.status;
    hdr.flags     = 0;
    hdr.reserved  = 0;
    hdr.value_len = static_cast<uint32_t>(resp.value.size());

    std::vector<uint8_t> buf(sizeof(hdr) + resp.value.size());
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    std::memcpy(buf.data() + sizeof(hdr), resp.value.data(), resp.value.size());
    return buf;
}

// ---------------------------------------------------------------------------
// Deserialization helpers (parse from contiguous byte ranges)
// ---------------------------------------------------------------------------

inline std::optional<Request> parse_request(const uint8_t* data, size_t len) {
    if (len < sizeof(RequestHeader)) return std::nullopt;

    RequestHeader hdr;
    std::memcpy(&hdr, data, sizeof(hdr));

    size_t payload_size = static_cast<size_t>(hdr.key_len) + hdr.value_len;
    if (len < sizeof(hdr) + payload_size) return std::nullopt;

    const char* payload = reinterpret_cast<const char*>(data + sizeof(hdr));
    Request req;
    req.opcode = hdr.opcode;
    req.key.assign(payload, hdr.key_len);
    req.value.assign(payload + hdr.key_len, hdr.value_len);
    return req;
}

inline std::optional<Response> parse_response(const uint8_t* data, size_t len) {
    if (len < sizeof(ResponseHeader)) return std::nullopt;

    ResponseHeader hdr;
    std::memcpy(&hdr, data, sizeof(hdr));

    if (len < sizeof(hdr) + hdr.value_len) return std::nullopt;

    const char* payload = reinterpret_cast<const char*>(data + sizeof(hdr));
    Response resp;
    resp.status = hdr.status;
    resp.value.assign(payload, hdr.value_len);
    return resp;
}

}  // namespace pensieve
