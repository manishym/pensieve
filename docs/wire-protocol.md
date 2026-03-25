# Wire Protocol Reference

Pensieve uses a compact binary protocol over TCP for all cache operations. This document specifies the on-wire format so you can build a client in any language.

## Connection Model

- Open a TCP connection to any node's **data port** (default 11211).
- The connection is persistent: send multiple request/response pairs on the same socket.
- The protocol is **request-response** (not multiplexed): send one request, read one response, then send the next.
- The coordinator node transparently proxies requests to the correct owner if the key hashes to a different node.

---

## Request Frame

```
 0      1      2      3      4      5      6      7
+------+------+------+------+------+------+------+------+
|opcode| flags|   key_len   |          value_len         |
+------+------+------+------+------+------+------+------+
|                     key (key_len bytes)                 |
+--------------------------------------------------------+
|                   value (value_len bytes)               |
+--------------------------------------------------------+
```

### Request Header (8 bytes, fixed)

| Offset | Size | Field | Type | Description |
| :--- | :--- | :--- | :--- | :--- |
| 0 | 1 | `opcode` | `uint8_t` | Operation code (see below) |
| 1 | 1 | `flags` | `uint8_t` | Reserved, set to `0` |
| 2 | 2 | `key_len` | `uint16_t` | Length of the key in bytes (little-endian) |
| 4 | 4 | `value_len` | `uint32_t` | Length of the value in bytes (little-endian) |

### Opcodes

| Code | Name | Description |
| :--- | :--- | :--- |
| `0x01` | `GET` | Retrieve the value for a key |
| `0x02` | `PUT` | Store a key-value pair |
| `0x03` | `DEL` | Delete a key |

### Request Body

Immediately following the 8-byte header:

1. **Key**: `key_len` bytes of raw key data.
2. **Value**: `value_len` bytes of raw value data.

For `GET` and `DEL`, `value_len` is `0` and no value bytes follow.

---

## Response Frame

```
 0      1      2      3      4      5      6      7
+------+------+------+------+------+------+------+------+
|status| flags|  reserved   |          value_len         |
+------+------+------+------+------+------+------+------+
|                   value (value_len bytes)               |
+--------------------------------------------------------+
```

### Response Header (8 bytes, fixed)

| Offset | Size | Field | Type | Description |
| :--- | :--- | :--- | :--- | :--- |
| 0 | 1 | `status` | `uint8_t` | Status code (see below) |
| 1 | 1 | `flags` | `uint8_t` | Reserved, set to `0` |
| 2 | 2 | `reserved` | `uint16_t` | Reserved, set to `0` |
| 4 | 4 | `value_len` | `uint32_t` | Length of the value in bytes (little-endian) |

### Status Codes

| Code | Name | Meaning |
| :--- | :--- | :--- |
| `0x00` | `OK` | Operation succeeded |
| `0x01` | `NOT_FOUND` | Key does not exist (GET) or was already absent (DEL) |
| `0x02` | `ERROR` | Internal error (storage full, peer unreachable, etc.) |

### Response Body

If `value_len > 0`, exactly `value_len` bytes of value data follow the header. This is typically present only for successful `GET` responses.

---

## Operation Semantics

### GET

| Field | Value |
| :--- | :--- |
| Request opcode | `0x01` |
| Request key | the key to look up |
| Request value_len | `0` |
| Response (hit) | status=`OK`, value=stored bytes |
| Response (miss) | status=`NOT_FOUND`, value_len=`0` |

### PUT

| Field | Value |
| :--- | :--- |
| Request opcode | `0x02` |
| Request key | the key to store |
| Request value | the value to store |
| Response (success) | status=`OK`, value_len=`0` |
| Response (failure) | status=`ERROR`, value_len=`0` |

PUT is an upsert: it creates the key if absent, or overwrites the existing value. Failure occurs when the slab allocator cannot find space even after eviction attempts.

### DEL

| Field | Value |
| :--- | :--- |
| Request opcode | `0x03` |
| Request key | the key to delete |
| Request value_len | `0` |
| Response (existed) | status=`OK`, value_len=`0` |
| Response (absent) | status=`NOT_FOUND`, value_len=`0` |

---

## Byte Order

All multi-byte fields (`key_len`, `value_len`, `reserved`) are in **native byte order** (typically little-endian on x86-64/ARM64 Linux). Since Pensieve targets Linux exclusively, and `memcpy`-based serialization is used, the wire format matches the host's native layout.

For portable clients on little-endian platforms, no byte-swapping is needed. On big-endian platforms (rare), swap `key_len` (2 bytes) and `value_len` (4 bytes) to little-endian before sending.

---

## Size Limits

| Limit | Value | Reason |
| :--- | :--- | :--- |
| Max key size | 65,535 bytes | `uint16_t key_len` |
| Max value size | ~1 MB | Largest slab class is 1,048,576 bytes |
| Max total payload | ~1 MB + 64 KB | `key_len + value_len` per request |

---

## Example: Python Client

A minimal client using raw sockets:

```python
import socket
import struct

def make_request(opcode, key, value=b""):
    key_bytes = key.encode() if isinstance(key, str) else key
    header = struct.pack("<BBhI", opcode, 0, len(key_bytes), len(value))
    return header + key_bytes + value

def parse_response(sock):
    hdr = sock.recv(8)
    status, flags, reserved, value_len = struct.unpack("<BBhI", hdr)
    value = b""
    if value_len > 0:
        value = b""
        while len(value) < value_len:
            chunk = sock.recv(value_len - len(value))
            if not chunk:
                raise ConnectionError("peer closed")
            value += chunk
    return status, value

GET, PUT, DEL = 1, 2, 3
OK, NOT_FOUND, ERROR = 0, 1, 2

# Connect
sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(("127.0.0.1", 11211))

# PUT
sock.sendall(make_request(PUT, "hello", b"world"))
status, _ = parse_response(sock)
assert status == OK

# GET
sock.sendall(make_request(GET, "hello"))
status, value = parse_response(sock)
assert status == OK
assert value == b"world"

# DEL
sock.sendall(make_request(DEL, "hello"))
status, _ = parse_response(sock)
assert status == OK

# GET (miss)
sock.sendall(make_request(GET, "hello"))
status, value = parse_response(sock)
assert status == NOT_FOUND

sock.close()
print("All operations succeeded.")
```

---

## Example: Bash Client (netcat)

For quick smoke tests, use `printf` and `xxd`:

```bash
# PUT key="hi" value="there"
# Header: opcode=0x02 flags=0x00 key_len=0x0200 value_len=0x05000000
printf '\x02\x00\x02\x00\x05\x00\x00\x00hithere' | nc -q1 127.0.0.1 11211 | xxd

# GET key="hi"
# Header: opcode=0x01 flags=0x00 key_len=0x0200 value_len=0x00000000
printf '\x01\x00\x02\x00\x00\x00\x00\x00hi' | nc -q1 127.0.0.1 11211 | xxd
```

---

## Pipelining

Multiple requests can be sent on the same connection sequentially. After sending a request, wait for the complete response (header + value bytes) before sending the next request. True multiplexed pipelining (sending multiple requests before reading responses) is not supported in this alpha release.

---

## Error Handling

- If the TCP connection drops mid-request, the server closes the connection. Clients should reconnect.
- If a proxied request to a peer node fails (peer unreachable, connection refused), the coordinator returns status `ERROR`.
- Truncated headers or payloads (connection closed before all bytes arrive) result in connection termination.
