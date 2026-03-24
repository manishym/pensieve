# Pensieve

**A decentralized, high-performance distributed cache built in C++20.**

Pensieve is an infrastructure-level key-value store that eliminates central points of failure through a **Shared-Nothing** architecture. Every node acts as both a storage engine and a request coordinator, enabling horizontal scaling without master election or consensus bottlenecks.

---

## Why Pensieve?

| Design Principle | Approach |
| :--- | :--- |
| **Kernel Bypass I/O** | `io_uring`-based async reactor — minimal syscalls, zero-copy buffers |
| **Decentralized Membership** | SWIM gossip protocol — no Zookeeper, no etcd, no single point of failure |
| **Deterministic Latency** | Slab allocator over HugePages — no heap fragmentation, no GC pauses |
| **Linear Scalability** | Thread-per-Core model with sharded maps — zero cross-core contention |

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────┐
│                      Client Request                     │
└──────────────────────────┬──────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────┐
│                  Coordinator Node                        │
│  ┌────────────┐  ┌──────────────┐  ┌─────────────────┐  │
│  │  io_uring   │  │  Hash Ring   │  │  Slab Storage   │  │
│  │  Reactor    │  │  Lookup      │  │  Engine         │  │
│  └─────┬──────┘  └──────┬───────┘  └────────┬────────┘  │
│        │                │                    │           │
│        │    ┌───────────┴───────────┐        │           │
│        │    │ Key is local? ────Yes──┼────────┘           │
│        │    │       │               │                    │
│        │    │      No               │                    │
│        │    │       ▼               │                    │
│        │    │  Async Proxy ─────────┘                    │
│        │    │  (co_await + io_uring)                     │
│        │    └───────────────────────┘                    │
└──────────────────────────────────────────────────────────┘
                           │
              ┌────────────┼────────────┐
              ▼            ▼            ▼
         ┌────────┐  ┌────────┐  ┌────────┐
         │ Peer A │  │ Peer B │  │ Peer C │
         │ (SWIM) │  │ (SWIM) │  │ (SWIM) │
         └────────┘  └────────┘  └────────┘
```

### Core Components

**Network & I/O Layer** — A single `io_uring` instance per CPU core drives all async TCP (data) and UDP (control) traffic. Registered buffer pools enable zero-copy transfers between kernel and userspace.

**Membership Layer (SWIM Gossip)** — Nodes discover and monitor each other via UDP heartbeats with indirect probing. Membership changes (join/leave/suspect) piggyback on gossip packets, keeping control-plane bandwidth minimal.

**Consistent Hash Ring** — Each physical node maps to 128–256 virtual points (via MurmurHash3) to eliminate hotspots. Ring lookups use `std::map::lower_bound` with wrap-around semantics.

**Storage Engine (Slab Allocator)** — A contiguous `mmap` region divided into power-of-two slab classes (64B–1MB). All allocations are 64-byte aligned to prevent false sharing. Eviction follows a Clock (Second Chance) policy.

**Request Coordinator** — Any node can proxy requests for keys it doesn't own. The coordinator suspends the request via C++20 coroutines, forwards to the owning peer over `io_uring`, and resumes on completion — fully transparent to the client.

---

## System Invariants

| Scenario | Behavior |
| :--- | :--- |
| **Thundering Herd** | Concurrent requests for the same missing key collapse into a single upstream fetch (Wait Groups) |
| **Node Join / Leave** | Only keys in the shifted hash range migrate between neighbors (Range Migration) |
| **Network Partition** | Prioritizes **Availability (AP)** — nodes continue serving local data with a temporarily fragmented ring view |

---

## Technology Stack

| Component | Choice |
| :--- | :--- |
| Language | C++20 (`std::coroutine`, `std::atomic`) |
| Kernel Interface | `liburing` (Linux Kernel 5.10+) |
| Serialization | FlatBuffers (zero-copy binary) |
| Hashing | MurmurHash3 (64-bit optimized) |

---

## Roadmap

The project is organized into five epics, building from the I/O foundation up to full distributed coordination:

1. **High-Performance I/O Foundation** — `io_uring` reactor, async TCP server, registered buffer pools, C++20 coroutine integration
2. **Decentralized Membership** — UDP heartbeating, SWIM failure detection, infection-style dissemination, ring store
3. **Consistent Hashing & Virtual Nodes** — MurmurHash3 integration, virtual node mapping, ring lookup, range migration
4. **Storage Engine** — Slab allocator over HugePages, sharded local map, Clock/LRU eviction, cache-line optimization
5. **Request Coordination & Proxying** — Async coordinator state machine, zero-copy proxying, request collapsing

See [`docs/epics.md`](docs/epics.md) for detailed stories and [`docs/architecture.md`](docs/architecture.md) for the full design document.

---

## Platform Requirements

- Linux Kernel 5.10+ (for `io_uring` support)
- C++20-compatible compiler (GCC 11+ / Clang 14+)
- `liburing` development headers
