#pragma once

#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <endian.h>

namespace pensieve {

enum class Opcode : uint8_t { Get = 0x00, Set = 0x01, Del = 0x04, ClusterInfo = 0x0A };
enum class Status : uint16_t { Ok = 0x0000, NotFound = 0x0001, Error = 0x0008 };

// On-wire layout: 24 bytes, big-endian, no padding.
struct alignas(1) MemHeader {
    uint8_t magic;      // Req: 0x80 | Res: 0x81
    uint8_t opcode;     // GET: 0x00 | SET: 0x01 | DEL: 0x04 | ClusterInfo: 0x0A
    uint16_t key_len;   // Key string length
    uint8_t ext_len;    // Extras length
    uint8_t data_type;  // 0x00
    uint16_t vbucket;   // Request: vbucket, Response: status
    uint32_t body_len;  // (key + ext + value)
    uint32_t opaque;    // Echo to client
    uint64_t cas;       // Versioning
} __attribute__((packed));

static_assert(sizeof(MemHeader) == 24);

// Zero-copy parsed request
struct Request {
    Opcode opcode;
    std::string_view key;
    std::string_view value;
    uint32_t opaque = 0;
    uint64_t cas = 0;
};

// Response with owned value data
struct Response {
    Status status;
    std::string value;
    uint32_t opaque = 0;
    uint64_t cas = 0;
};

// ---------------------------------------------------------------------------
// Serialization helpers (write into byte vectors or parse headers)
// ---------------------------------------------------------------------------

inline std::vector<uint8_t> serialize_request(const Request& req) {
    MemHeader hdr{};
    hdr.magic = 0x80;
    hdr.opcode = static_cast<uint8_t>(req.opcode);
    hdr.key_len = htobe16(static_cast<uint16_t>(req.key.size()));
    hdr.ext_len = 0;
    hdr.data_type = 0;
    hdr.vbucket = 0;
    hdr.body_len = htobe32(static_cast<uint32_t>(req.key.size() + req.value.size()));
    hdr.opaque = htobe32(req.opaque);
    hdr.cas = htobe64(req.cas);

    std::vector<uint8_t> buf(sizeof(hdr) + req.key.size() + req.value.size());
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    if (!req.key.empty()) {
        std::memcpy(buf.data() + sizeof(hdr), req.key.data(), req.key.size());
    }
    if (!req.value.empty()) {
        std::memcpy(buf.data() + sizeof(hdr) + req.key.size(),
                    req.value.data(), req.value.size());
    }
    return buf;
}

inline std::vector<uint8_t> serialize_response(const Response& resp) {
    MemHeader hdr{};
    hdr.magic = 0x81;
    hdr.opcode = 0x00; // Opcodes usually aren't matched closely on standard response, but we can set to 0.
    hdr.key_len = 0;
    hdr.ext_len = 0;
    hdr.data_type = 0;
    hdr.vbucket = htobe16(static_cast<uint16_t>(resp.status));
    hdr.body_len = htobe32(static_cast<uint32_t>(resp.value.size()));
    hdr.opaque = htobe32(resp.opaque);
    hdr.cas = htobe64(resp.cas);

    std::vector<uint8_t> buf(sizeof(hdr) + resp.value.size());
    std::memcpy(buf.data(), &hdr, sizeof(hdr));
    if (!resp.value.empty()) {
        std::memcpy(buf.data() + sizeof(hdr), resp.value.data(), resp.value.size());
    }
    return buf;
}

// ---------------------------------------------------------------------------
// Deserialization helpers (parse from contiguous byte ranges)
// ---------------------------------------------------------------------------

inline std::optional<Request> parse_request(const uint8_t* data, size_t len) {
    if (len < sizeof(MemHeader)) return std::nullopt;

    MemHeader hdr;
    std::memcpy(&hdr, data, sizeof(hdr));

    if (hdr.magic != 0x80) return std::nullopt;

    uint16_t key_len = be16toh(hdr.key_len);
    uint32_t body_len = be32toh(hdr.body_len);
    uint8_t ext_len = hdr.ext_len;

    if (len < sizeof(MemHeader) + body_len) return std::nullopt;

    Request req;
    req.opcode = static_cast<Opcode>(hdr.opcode);
    req.opaque = be32toh(hdr.opaque);
    req.cas = be64toh(hdr.cas);

    const char* payload = reinterpret_cast<const char*>(data + sizeof(hdr));
    if (key_len > 0) req.key = std::string_view(payload + ext_len, key_len);

    if (static_cast<uint32_t>(ext_len) + static_cast<uint32_t>(key_len) > body_len) return std::nullopt;
    uint32_t value_len = body_len - ext_len - key_len;
    if (value_len > 0) req.value = std::string_view(payload + ext_len + key_len, value_len);

    return req;
}

inline std::optional<Response> parse_response(const uint8_t* data, size_t len) {
    if (len < sizeof(MemHeader)) return std::nullopt;

    MemHeader hdr;
    std::memcpy(&hdr, data, sizeof(hdr));

    if (hdr.magic != 0x81) return std::nullopt;

    uint32_t body_len = be32toh(hdr.body_len);
    if (len < sizeof(hdr) + body_len) return std::nullopt;

    Response resp;
    resp.status = static_cast<Status>(be16toh(hdr.vbucket));
    resp.opaque = be32toh(hdr.opaque);
    resp.cas = be64toh(hdr.cas);

    const char* payload = reinterpret_cast<const char*>(data + sizeof(hdr));
    if (body_len > 0) {
        resp.value.assign(payload, body_len);
    }
    return resp;
}

}  // namespace pensieve
