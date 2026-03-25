# Project DCC: Feature Epics & Technical Stories

## Epic 1: High-Performance I/O Foundation (`io_uring` Reactor)
**Goal:** Establish a non-blocking, kernel-bypass event loop to handle high-concurrency TCP/UDP traffic with minimal syscall overhead.

* **Story 1.1: The Ring Abstraction** Implement a C++ wrapper for `liburing` to manage the Submission Queue (SQ) and Completion Queue (CQ) setup.
* **Story 1.2: Async TCP Server** Create a `TcpServer` that handles `accept`, `read`, and `write` operations as asynchronous CQEs (Completion Queue Events).
* **Story 1.3: Registered Buffer Pool** Implement a fixed memory pool using `IORING_REGISTER_BUFFERS` to enable zero-copy data transfers between the kernel and userspace.
* **Story 1.4: Coroutine Integration** Develop C++20 `awaitable` types to wrap `io_uring` operations, allowing linear code flow for complex async I/O.

---gco 

## Epic 2: Decentralized Membership (SWIM Gossip)
**Goal:** Implement a platform-agnostic, peer-to-peer discovery model to manage the cluster state without a central master.

* **Story 2.1: UDP Heartbeating** Implement a low-latency UDP-based `Ping/Ack` mechanism to track basic peer availability.
* **Story 2.2: SWIM Failure Detector** Build "Indirect Probing" logic to verify node health via third-party peers, mitigating false positives caused by network congestion.
* **Story 2.3: Infection-style Dissemination** Piggyback membership updates (Join/Leave/Suspect) onto existing Gossip packets to minimize control-plane bandwidth.
* **Story 2.4: The Ring Store** Implement the `std::map<uint32_t, NodeInfo>` that maintains the global hash-to-node mapping on every peer.

---

## Epic 3: Consistent Hashing & Virtual Nodes
**Goal:** Ensure deterministic data distribution and minimize re-sharding overhead during cluster membership changes.

* **Story 3.1: Systems-Grade Hashing** Integrate **MurmurHash3** (or CityHash) for high-entropy distribution of keys and node identifiers.
* **Story 3.2: Virtual Node Mapping** Map each physical node to 128/256 points on the hash ring to eliminate "hotspots" and ensure uniform load balancing.
* **Story 3.3: Ring Lookup Logic** Implement the `get_node_for_key` function using `std::map::lower_bound` with support for circular "Ring Wrap-around."
* **Story 3.4: Re-sharding & Range Migration** Develop the logic to identify which hash ranges must migrate between neighbors when a node joins or leaves the cluster.

---

## Epic 4: The Storage Engine (Slab & Local KV)
**Goal:** Build a deterministic, memory-efficient local store that avoids heap fragmentation and high-latency allocation paths.

* **Story 4.1: Slab Allocator Core** Implement a custom allocator using `mmap` and `MAP_HUGETLB` (HugePages) to reduce TLB misses and fragmentation.
* **Story 4.2: Local Sharded Map** Create a sharded `std::unordered_map` that stores metadata pointers into the pre-allocated Slab memory.
* **Story 4.3: Eviction Policy (Clock/LRU)** Implement a "Second Chance" (Clock) eviction algorithm within the Slab to handle memory exhaustion under load.
* **Story 4.4: Cache-Line Optimization** Apply 64-byte alignment and padding to internal structures to prevent **False Sharing** across CPU cores.

---

## Epic 5: Request Coordination & Proxying
**Goal:** Enable any node to act as a "Coordinator" for any client request, masking the distributed nature of the cache.

* **Story 5.1: Async Coordinator State Machine** Implement the logic to initiate an outbound `connect` and `send` to a peer if the hashed key is remote.
* **Story 5.2: Zero-Copy Proxying** Utilize reference-counted buffers so that data read from a Peer can be piped to a Client-Write without intermediate userspace copying.
* **Story 5.3: Request Collapsing (Wait Groups)** Implement a "Thundering Herd" protection layer: if $N$ requests arrive for the same missing key, only one upstream fetch is performed.

# Epic 6: The Smart SDK (Pensieve Client)

**Goal:** Develop a high-performance, ring-aware client library that enables $O(1)$ routing, connection pooling, and transparent failover without relying on server-side proxying.

---

## 6.1: Membership & Topology Discovery
The SDK must "learn" the cluster layout to make intelligent routing decisions.

* **Story 6.1.1: Bootstrap Logic:** Implement an initial connection phase where the SDK contacts a "Seed" node to fetch the current Hash Ring state.
* **Story 6.1.2: Ring Synchronization:** Create a background thread (or async task) that periodically refreshes the local `std::map<uint32_t, NodeInfo>` to detect new nodes or departures.
* **Story 6.1.3: Serialization Parity:** Implement a binary parser for the cluster metadata to ensure the SDK interprets the `VirtualNode` distribution exactly as the server intended.

---

## 6.2: Client-Side Consistent Hashing
The SDK must replicate the server's placement logic to ensure it hits the "Owner" node on the first try.

* **Story 6.2.1: Hashing Alignment:** Integrate `MurmurHash3` (or the chosen algorithm) with identical seeding and 64-bit/128-bit parity to the server implementation.
* **Story 6.2.2: Local Lookup Engine:** Implement the `find_node(key)` function using `std::map::lower_bound` to resolve keys to physical node IPs locally.
* **Story 6.2.3: Virtual Node Support:** Ensure the client correctly maps the 128/256 virtual points per node to prevent load imbalance.

---

## 6.3: Connection Pooling & I/O Efficiency
To match the performance of the `io_uring` backend, the SDK must avoid the overhead of constant TCP handshakes.

* **Story 6.3.1: Persistent Connection Pool:** Implement a thread-safe pool of TCP connections indexed by Node ID. Use "Keep-Alive" to reuse sockets for multiple requests.
* **Story 6.3.2: Non-Blocking I/O:** Build the SDK using asynchronous primitives (e.g., `std::future`, `asio`, or raw non-blocking sockets) so the calling application isn't stalled by network latency.
* **Story 6.3.3: Request Pipelining:** Allow the SDK to send multiple requests over a single connection without waiting for the previous response (Head-of-Line blocking mitigation).

---

## 6.4: Fault Tolerance & Self-Healing
The SDK acts as the first line of defense against cluster instability.

* **Story 6.4.1: Retries & Successor Fallback:** If the primary owner node returns a connection error, the SDK must automatically attempt the request on the "Next-Clockwise" node in the ring.
* **Story 6.4.2: Stale Map Detection:** Handle `MOVED` or `NOT_MY_KEY` errors from the server by immediately triggering a ring refresh and re-routing the request.
* **Story 6.4.3: Circuit Breaking:** Implement a mechanism to "blacklist" a failing node for $N$ seconds to prevent the application from constantly timing out on a dead peer.

---

## 6.5: Developer Experience (DX) & Observability
The SDK should be easy to integrate and monitor.

* **Story 6.5.1: The Pensieve CLI:** Create a command-line tool (e.g., `pensieve-cli`) that uses the SDK to perform `PUT`, `GET`, and `DELETE` operations for debugging.
* **Story 6.5.2: Telemetry:** Export key metrics such as `local_hash_time`, `network_latency_p99`, and `ring_out_of_sync_count`.