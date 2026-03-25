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

## Server Binary

The repository includes `src/main.cpp`, a production-ready server entry point. It is built automatically as `pensieve_server`:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
ls build/pensieve_server
```

The server reads configuration from environment variables (for Docker/container use) with CLI argument fallbacks:

| Variable | CLI arg | Default | Description |
| :--- | :--- | :--- | :--- |
| `PENSIEVE_HOST` | `$1` | `0.0.0.0` | Bind address / node identity |
| `PENSIEVE_DATA_PORT` | `$2` | `11211` | TCP data port |
| `PENSIEVE_GOSSIP_PORT` | `$3` | `7946` | UDP gossip port |
| `PENSIEVE_MEMORY_MB` | `$4` | `64` | Slab allocator size in MB |
| `PENSIEVE_SEED` | `$5` | (none) | Seed node as `host:gossip_port` |
| `PENSIEVE_SEED_DATA_PORT` | - | same as data_port | Seed node's data port |

Hostnames are automatically resolved to IPv4 addresses.

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

---

## Docker Compose Deployments

Pre-built Docker Compose configurations are provided in the `deploy/` directory for 3-node and 5-node clusters. See [`deploy/README.md`](../deploy/README.md) for full instructions.

Quick start:

```bash
# Build and start a 3-node cluster
docker compose -f deploy/docker-compose.3-node.yml up --build

# In another terminal, test it
python3 deploy/client.py 127.0.0.1 11211 PUT hello world
python3 deploy/client.py 127.0.0.1 11212 GET hello
```
