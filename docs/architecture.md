# Architecture: Distributed Consistent Cache (Pensieve)

## 1. Executive Summary
Pensieve is a high-performance, decentralized key-value store designed for infrastructure-level caching. It eliminates central points of failure by using a **Shared-Nothing** architecture where every node acts as both a storage engine and a request coordinator.

### Key Performance Drivers:
* **Kernel Bypass (io_uring):** Minimizes context switches and syscall overhead.
* **Zero-Copy Data Path:** Uses `IORING_REGISTER_BUFFERS` and reference-counted Slab memory.
* **Lock-Free Membership:** Eventually consistent hash ring managed via the SWIM protocol.

---

## 2. System Components

### 2.1 Network & I/O Layer (`io_uring`)
The system employs a **Thread-per-Core (TPC)** model to maximize cache locality and minimize cross-core synchronization.
* **Async Reactor:** A single `io_uring` instance per physical CPU core.
* **Completion Queue (CQ) Processing:** CQEs are handled via a non-blocking event loop.
* **Multi-Protocol Support:** * **TCP (Data):** For client requests and Peer-to-Peer (P2P) proxying.
    * **UDP (Control):** For Gossip/SWIM heartbeats to avoid TCP head-of-line blocking for metadata.

### 2.2 Membership Layer (SWIM Gossip)
To remain platform-agnostic (K8s, Bare Metal, or Cloud), Pensieve implements the **SWIM Protocol**.
* **The Ring Store:** A `std::map<uint32_t, NodeInfo>` representing the consistent hash ring.
* **Failure Detection:** Probabilistic ping-req mechanism to differentiate between node death and network congestion.
* **Virtual Nodes:** Each physical node maps to 128-256 virtual points on the ring to ensure uniform distribution and mitigate "hotspots."

### 2.3 Storage Engine (Slab Allocator)
To achieve deterministic latency and avoid heap fragmentation:
* **Memory Pool:** Contiguous `mmap` region allocated at process start.
* **Slab Classes:** Divided into power-of-two buckets (64B to 1MB).
* **Alignment:** All allocations are 64-byte aligned to prevent **False Sharing** on CPU cache lines.

---

## 3. Request Lifecycle (The Data Path)

### 3.1 Coordinator Logic
When a node receives a request for a key it does not own:
1. **Suspension:** The current request context is captured in a **C++20 Coroutine (`co_await`)**.
2. **Asynchronous Proxy:** An outbound `connect` and `send` are submitted to the `io_uring` SQ.
3. **Resumption:** When the peer responds, the `CQE` triggers the coroutine resumption, and the result is streamed back to the original client.

### 3.2 Concurrency & Locking
* **Read-Copy-Update (RCU):** The Hash Ring (`std::map`) is updated using RCU-like semantics. Lookups are lock-free; updates (Gossip) happen on a separate version of the map before being swapped via an atomic pointer.
* **Sharded Map:** The local Key-Value store is sharded by hash to allow multiple worker threads to access the local data with zero contention.

---

## 4. Systems Invariants

| Scenario | System Response |
| :--- | :--- |
| **Thundering Herd** | If $N$ requests arrive for a missing key, the node creates a single "Wait Group" and performs one upstream fetch. |
| **Node Join/Leave** | Triggers a "Range Migration." Only the keys belonging to the shifted hash space are moved between neighbors. |
| **Network Partition** | Prioritizes **Availability (AP)**. Nodes continue to serve local data even if the global ring view is temporarily fragmented. |

---

## 5. Technology Stack
* **Language:** C++20 (utilizing `std::coroutine` and `std::atomic`).
* **Kernel Interface:** `liburing` (Linux Kernel 5.10+).
* **Serialization:** FlatBuffers (Zero-copy binary format).
* **Hashing:** MurmurHash3 (Optimized for 64-bit architectures).