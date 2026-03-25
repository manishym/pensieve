# Deployment & Usage Guide

This guide covers building Pensieve from source, writing a server entry point, forming a cluster, and operating it in production.

## Prerequisites

| Requirement | Minimum Version | Notes |
| :--- | :--- | :--- |
| Linux Kernel | 5.10+ | `io_uring` support required |
| C++ Compiler | GCC 11 / Clang 14 | C++20 coroutines, `<bit>`, structured bindings |
| CMake | 3.22+ | Build system |
| liburing | 2.0+ | `apt install liburing-dev` on Debian/Ubuntu |
| pkg-config | any | Used by CMake to locate liburing |

HugePages are optional but recommended for the slab allocator:

```bash
# Allocate 512 x 2MB huge pages (1 GB total)
echo 512 | sudo tee /proc/sys/vm/nr_hugepages

# Verify
grep HugePages /proc/meminfo
```

---

## Building from Source

```bash
git clone https://github.com/manishym/pensieve.git
cd pensieve

cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

This produces five static libraries under `build/`:

| Library | Contains |
| :--- | :--- |
| `libpensieve_io.a` | io_uring reactor, TCP/UDP, buffer pool, coroutine runtime |
| `libpensieve_hash.a` | MurmurHash3 |
| `libpensieve_membership.a` | SWIM gossip, member list, ring store, disseminator |
| `libpensieve_storage.a` | Slab allocator, local sharded map, clock evictor |
| `libpensieve_coordinator.a` | Request coordinator, peer client, wait group |

### Running Tests

```bash
cd build && ctest --output-on-failure
```

All 25 test suites should pass. Tests exercise every layer from raw `io_uring` operations through full coordinator integration with remote proxying.

---

## Writing a Server Entry Point

Pensieve ships as a set of composable libraries. To run a node you write a short `main.cpp` that wires the components together. Below is a complete, production-ready example.

### `src/main.cpp`

```cpp
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>

#include "common/types.h"
#include "coordinator/coordinator.h"
#include "io/awaitable.h"
#include "io/buffer_pool.h"
#include "io/io_uring_context.h"
#include "io/task.h"
#include "io/tcp_listener.h"
#include "io/udp_socket.h"
#include "membership/disseminator.h"
#include "membership/member_list.h"
#include "membership/node_info.h"
#include "membership/ring_store.h"
#include "membership/swim_protocol.h"
#include "storage/local_store.h"

using namespace pensieve;

static IoUringContext* g_ctx = nullptr;

void signal_handler(int) {
    if (g_ctx) g_ctx->stop();
}

int main(int argc, char* argv[]) {
    std::string host       = argc > 1 ? argv[1] : "0.0.0.0";
    uint16_t data_port     = argc > 2 ? uint16_t(std::atoi(argv[2])) : 11211;
    uint16_t gossip_port   = argc > 3 ? uint16_t(std::atoi(argv[3])) : 7946;
    size_t   memory_mb     = argc > 4 ? std::stoul(argv[4]) : 256;
    std::string seed_host  = argc > 5 ? argv[5] : "";
    uint16_t seed_gport    = argc > 6 ? uint16_t(std::atoi(argv[6])) : 7946;

    IoUringContext ctx(1024);
    g_ctx = &ctx;
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    BufferPool pool(ctx, 65536, 64);
    LocalStore store(memory_mb * 1024 * 1024, 16);

    NodeId self{host, gossip_port};
    MemberList members;
    RingStore ring;
    Disseminator disseminator(members);

    NodeInfo self_info;
    self_info.id = self;
    self_info.data_port = data_port;
    self_info.state = NodeState::Alive;
    members.add_node(self_info);
    ring.add_node(self);

    if (!seed_host.empty()) {
        NodeId seed{seed_host, seed_gport};
        NodeInfo seed_info;
        seed_info.id = seed;
        seed_info.state = NodeState::Alive;
        members.add_node(seed_info);
        ring.add_node(seed);
    }

    UdpSocket gossip_sock(ctx, host, gossip_port);
    SwimProtocol::Config swim_cfg;
    SwimProtocol swim(ctx, gossip_sock, members, disseminator,
                      self, swim_cfg);

    Coordinator coordinator(ctx, store, ring, members, self, &pool);

    TcpListener listener(ctx, host, data_port);
    listener.start([&](TcpConnection conn) {
        auto task = coordinator.handle_connection(conn.release_fd());
        task.start();
    });

    swim.run();
    std::cout << "pensieve listening on " << host
              << " data=" << data_port
              << " gossip=" << gossip_port
              << " memory=" << memory_mb << "MB\n";
    ctx.run();

    swim.stop();
    return 0;
}
```

### Building with the server binary

Add to your `CMakeLists.txt`:

```cmake
add_executable(pensieve_server src/main.cpp)
target_link_libraries(pensieve_server PRIVATE
    pensieve_coordinator pensieve_storage
    pensieve_membership pensieve_hash pensieve_io)
```

Then rebuild:

```bash
cmake --build build -j$(nproc)
```

---

## Running a Node

```
./build/pensieve_server <host> <data_port> <gossip_port> <memory_mb> [seed_host] [seed_gossip_port]
```

### Single-node (development)

```bash
./build/pensieve_server 127.0.0.1 11211 7946 256
```

### Three-node cluster

Terminal 1 (seed node):

```bash
./build/pensieve_server 192.168.1.10 11211 7946 1024
```

Terminal 2 (joins via seed):

```bash
./build/pensieve_server 192.168.1.11 11211 7946 1024 192.168.1.10 7946
```

Terminal 3 (joins via seed):

```bash
./build/pensieve_server 192.168.1.12 11211 7946 1024 192.168.1.10 7946
```

Once node 2 and node 3 join, SWIM gossip propagates the full membership to all nodes within a few protocol periods (1-3 seconds). After convergence, every node's hash ring contains all three nodes and requests are routed accordingly.

---

## Ports

Each Pensieve node uses two ports:

| Port | Protocol | Purpose |
| :--- | :--- | :--- |
| **Data port** (default 11211) | TCP | Client requests (GET/PUT/DEL) and peer-to-peer proxying |
| **Gossip port** (default 7946) | UDP | SWIM protocol heartbeats and membership updates |

Ensure both ports are open in your firewall between all cluster nodes. Only the data port needs to be exposed to application clients.

---

## Cluster Formation

Pensieve uses the **SWIM gossip protocol** for fully decentralized membership. There is no central coordinator, no etcd, and no Zookeeper.

### Bootstrap sequence

1. **First node** starts with no seed. It registers itself on the hash ring and begins listening for gossip and data traffic.
2. **Subsequent nodes** start with the `seed_host`/`seed_gossip_port` of any existing member. On startup they add the seed to their local member list.
3. **SWIM protocol** runs periodic probe cycles (default every 1 second):
   - Direct **Ping/Ack** to a random peer
   - **Indirect ping** (Ping-Req) via third-party nodes if direct ping times out
   - **Infection-style dissemination**: membership updates (Join/Leave/Suspect) piggyback on gossip packets
4. Within a few protocol periods, all nodes converge on a consistent membership view and hash ring.

### Failure detection

| Event | Detection | Recovery |
| :--- | :--- | :--- |
| Node unresponsive | Direct ping timeout (200ms) triggers indirect probing | If indirect probes also fail, node marked **Suspect** |
| Suspect timeout | After 5s without recovery | Node marked **Dead**, removed from hash ring |
| Network partition | Split-brain possible | AP model: each partition continues serving local data |

### Node identity

A node is uniquely identified by its `{host, gossip_port}` tuple. The `data_port` is metadata carried in `NodeInfo` and communicated via gossip.

---

## Configuration Reference

All configuration is currently via constructor parameters or CLI arguments.

### Storage

| Parameter | Default | Description |
| :--- | :--- | :--- |
| `total_memory` | 256 MB | Total slab allocator capacity. Uses HugePages when available. |
| `num_shards` | 16 | Number of independent lock-sharded partitions in the local map |

Slab classes range from **64 bytes to 1 MB** in power-of-two increments (15 size classes). Entries that exceed 1 MB cannot be stored.

### SWIM Gossip

| Parameter | Default | Description |
| :--- | :--- | :--- |
| `protocol_period` | 1000 ms | Interval between probe rounds |
| `ping_timeout` | 200 ms | Direct ping deadline before escalating to indirect probing |
| `suspect_timeout` | 5000 ms | Time in Suspect state before declaring Dead |
| `indirect_ping_peers` | 3 | Number of peers used for indirect ping-req |
| `max_piggyback_updates` | 8 | Max membership updates piggybacked per gossip packet |

### Hash Ring

| Parameter | Default | Description |
| :--- | :--- | :--- |
| `num_vnodes` | 128 | Virtual nodes per physical node on the consistent hash ring |

### Buffer Pool (Zero-Copy)

| Parameter | Default | Description |
| :--- | :--- | :--- |
| `buffer_size` | 64 KB | Size of each registered `io_uring` buffer |
| `buffer_count` | 64 | Number of buffers in the pool |

### io_uring

| Parameter | Default | Description |
| :--- | :--- | :--- |
| `queue_depth` | 256 | Submission/completion queue depth per io_uring instance |

---

## Operations

### Health Checking

Currently Pensieve does not expose an HTTP health endpoint. To verify a node is alive:

- **TCP probe**: Connect to the data port and send a GET for a known key.
- **Gossip probe**: The SWIM protocol itself serves as a distributed health check. A node that appears in peers' member lists with state `Alive` is healthy.

### Monitoring

Key internal metrics available via the C++ API:

| Metric | Source |
| :--- | :--- |
| `local_store.size()` | Number of keys stored locally |
| `local_store.memory_used()` | Bytes consumed by slab allocator |
| `member_list.size()` | Number of known cluster members |
| `ring_store.size()` | Number of virtual node tokens on the ring |
| `buffer_pool.available()` | Free registered buffers for zero-copy I/O |

### Graceful Shutdown

Send `SIGINT` or `SIGTERM` to the process. The signal handler calls `ctx.stop()`, which drains the `io_uring` event loop and terminates the process.

---

## Limitations (Alpha)

This is an alpha release. Known limitations:

| Area | Limitation |
| :--- | :--- |
| **Persistence** | In-memory only. All data is lost on restart. |
| **Replication** | No data replication between nodes. A node failure loses its partition. |
| **Authentication** | No auth or TLS. Deploy behind a private network or VPN. |
| **Max value size** | 1 MB (largest slab class). Larger values are rejected. |
| **Max key size** | 65,535 bytes (`uint16_t key_len` in the wire protocol header). |
| **Concurrency model** | Single-threaded event loop. For multi-core, run one process per core with separate port ranges. |
| **Seed discovery** | Manual seed node configuration. No DNS-based or cloud-native discovery. |
| **Observability** | No Prometheus endpoint or structured logging. |
| **Client libraries** | No official client SDKs. Clients implement the binary wire protocol directly. |

See the [Wire Protocol Reference](wire-protocol.md) for details on building a client.
