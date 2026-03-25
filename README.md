# Pensieve

**A decentralized, high-performance distributed cache built in C++20.**

Pensieve is an infrastructure-level key-value store that eliminates central points of failure through a **Shared-Nothing** architecture. Every node acts as both a storage engine and a request coordinator, enabling horizontal scaling without master election or consensus bottlenecks.

---

## Why Pensieve?

| Design Principle | Approach |
| :--- | :--- |
| **Kernel Bypass I/O** | `io_uring`-based async reactor with zero-copy registered buffers |
| **Decentralized Membership** | SWIM gossip protocol -- no Zookeeper, no etcd, no single point of failure |
| **Deterministic Latency** | Slab allocator over HugePages with no heap fragmentation or GC pauses |
| **Linear Scalability** | Sharded maps with per-shard locking for zero cross-core contention |

---

## Quick Start

### Prerequisites

- Linux Kernel 5.10+ (for `io_uring`)
- GCC 11+ or Clang 14+ (C++20)
- CMake 3.22+
- liburing 2.0+ (`apt install liburing-dev`)

### Build

```bash
git clone https://github.com/manishym/pensieve.git
cd pensieve
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

### Test

```bash
cd build && ctest --output-on-failure
```

25 test suites covering all layers: io_uring reactor, SWIM gossip, consistent hashing, slab storage, and coordinator proxying.

### Run a single node

See the [Deployment Guide](docs/deployment.md) for the server entry point code. Once built:

```bash
./build/pensieve_server 127.0.0.1 11211 7946 256
```

### Talk to it (Python)

```python
import socket, struct

sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
sock.connect(("127.0.0.1", 11211))

# PUT "hello" = "world"
key, val = b"hello", b"world"
sock.sendall(struct.pack("<BBhI", 2, 0, len(key), len(val)) + key + val)
status = struct.unpack("<BBhI", sock.recv(8))[0]
print(f"PUT: {'OK' if status == 0 else 'FAIL'}")

# GET "hello"
sock.sendall(struct.pack("<BBhI", 1, 0, len(key), 0) + key)
hdr = sock.recv(8)
status, _, _, vlen = struct.unpack("<BBhI", hdr)
value = sock.recv(vlen) if vlen else b""
print(f"GET: {value.decode()}")

sock.close()
```

---

## Architecture Overview

```
                        Client Request
                             |
                             v
  +--------------------------------------------------+
  |               Coordinator Node                    |
  |                                                   |
  |  io_uring       Hash Ring        Slab Storage     |
  |  Reactor        Lookup           Engine           |
  |     |              |                 |            |
  |     |    Key is local? ---Yes--------+            |
  |     |         |                                   |
  |     |        No                                   |
  |     |         v                                   |
  |     |    Async Proxy (co_await + io_uring)        |
  +--------------------------------------------------+
                             |
               +-------------+-------------+
               v             v             v
          +--------+    +--------+    +--------+
          | Peer A |    | Peer B |    | Peer C |
          | (SWIM) |    | (SWIM) |    | (SWIM) |
          +--------+    +--------+    +--------+
```

### Core Components

| Layer | What it does |
| :--- | :--- |
| **Network & I/O** | Single `io_uring` instance drives all async TCP and UDP traffic. Registered buffer pools enable zero-copy kernel-to-userspace transfers. |
| **Membership (SWIM)** | Nodes discover and monitor each other via UDP heartbeats with indirect probing. Membership changes piggyback on gossip packets. |
| **Consistent Hash Ring** | Each physical node maps to 128 virtual points (MurmurHash3). Lookups use `std::map::lower_bound` with wrap-around. |
| **Storage Engine** | Contiguous `mmap` region divided into 15 power-of-two slab classes (64B-1MB). 64-byte aligned to prevent false sharing. Clock eviction policy. |
| **Coordinator** | Any node proxies requests for keys it does not own. C++20 coroutines suspend the request, forward to the owner via a pooled TCP connection, and resume on completion. Request collapsing (Wait Groups) prevents thundering herds. |

---

## System Invariants

| Scenario | Behavior |
| :--- | :--- |
| **Thundering Herd** | Concurrent requests for the same missing key collapse into a single upstream fetch (Wait Groups) |
| **Node Join / Leave** | Only keys in the shifted hash range migrate between neighbors (Range Migration) |
| **Network Partition** | Prioritizes **Availability (AP)** -- nodes continue serving local data with a temporarily fragmented ring view |

---

## Wire Protocol

Pensieve uses a compact 8-byte binary header for requests and responses over TCP:

| Frame | Fields |
| :--- | :--- |
| **Request** (8 + N bytes) | `opcode(1) flags(1) key_len(2) value_len(4)` + key + value |
| **Response** (8 + N bytes) | `status(1) flags(1) reserved(2) value_len(4)` + value |

Operations: `GET(0x01)`, `PUT(0x02)`, `DEL(0x03)`.
Status codes: `OK(0x00)`, `NOT_FOUND(0x01)`, `ERROR(0x02)`.

See the full [Wire Protocol Reference](docs/wire-protocol.md) for implementation details and example clients.

---

## Documentation

| Document | Description |
| :--- | :--- |
| [Deployment Guide](docs/deployment.md) | Build, configure, deploy, form a cluster, operate |
| [Wire Protocol Reference](docs/wire-protocol.md) | Binary protocol spec with Python and Bash client examples |
| [Architecture](docs/architecture.md) | Design document covering all system components |
| [Epics & Stories](docs/epics.md) | Development roadmap and feature breakdown |

---

## Project Structure

```
src/
  common/          types.h, result.h
  io/              io_uring reactor, TCP/UDP, buffer pool, coroutine runtime
  hash/            MurmurHash3 implementation
  membership/      SWIM gossip, member list, ring store, disseminator
  storage/         Slab allocator, local sharded map, clock evictor
  coordinator/     Request coordinator, peer client, wait group
  protocol/        Binary wire protocol (message.h)
tests/
  io/              6 test suites
  membership/      8 test suites
  hash/            4 test suites
  storage/         4 test suites
  coordinator/     3 test suites
docs/
  architecture.md  System design document
  deployment.md    Deployment and usage guide
  wire-protocol.md Binary protocol specification
  epics.md         Feature epics and stories
```

---

## Technology Stack

| Component | Choice |
| :--- | :--- |
| Language | C++20 (`std::coroutine`, `<bit>`, structured bindings) |
| Kernel Interface | `liburing` (Linux Kernel 5.10+) |
| Hashing | MurmurHash3 (128-bit, 64-bit optimized) |
| Build | CMake 3.22+ with FetchContent (GoogleTest) |
| Testing | GoogleTest 1.14 (25 test suites) |

---

## Platform Requirements

- Linux Kernel 5.10+ (for `io_uring` support)
- C++20-compatible compiler (GCC 11+ / Clang 14+)
- `liburing` development headers
- Optional: HugePages for optimal slab allocator performance

---

## Alpha Limitations

- **In-memory only** -- no persistence, data lost on restart
- **No replication** -- node failure loses its partition
- **No auth/TLS** -- deploy behind a private network
- **1 MB max value** -- largest slab class
- **No official client SDKs** -- implement the binary protocol directly

---

## License

See [LICENSE](LICENSE) for details.
