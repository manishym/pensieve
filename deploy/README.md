# Docker Compose Deployments

Pre-built Docker Compose configurations for running Pensieve clusters locally.

## Prerequisites

- Docker Engine 20.10+ with Compose V2
- Linux host with kernel 5.10+ (io_uring support)

## Quick Start: 3-Node Cluster

```bash
docker compose -f deploy/docker-compose.3-node.yml up --build
```

This starts three Pensieve nodes:

| Node | Container IP | Host Port | Role |
| :--- | :--- | :--- | :--- |
| node1 | 172.30.0.11 | 11211 | Seed node |
| node2 | 172.30.0.12 | 11212 | Joins via node1 |
| node3 | 172.30.0.13 | 11213 | Joins via node1 |

After a few seconds, SWIM gossip converges and all nodes share the full ring.

### Test it

```bash
# Store a value via node1
python3 deploy/client.py 127.0.0.1 11211 PUT hello world

# Read it back via node2 (the coordinator proxies to the owner)
python3 deploy/client.py 127.0.0.1 11212 GET hello

# Read it via node3
python3 deploy/client.py 127.0.0.1 11213 GET hello

# Delete it via any node
python3 deploy/client.py 127.0.0.1 11211 DEL hello

# Confirm it's gone
python3 deploy/client.py 127.0.0.1 11212 GET hello
```

## 5-Node Cluster

```bash
docker compose -f deploy/docker-compose.5-node.yml up --build
```

| Node | Container IP | Host Port |
| :--- | :--- | :--- |
| node1 | 172.31.0.11 | 11211 |
| node2 | 172.31.0.12 | 11212 |
| node3 | 172.31.0.13 | 11213 |
| node4 | 172.31.0.14 | 11214 |
| node5 | 172.31.0.15 | 11215 |

Same usage as the 3-node cluster, with ports 11211-11215.

## How It Works

Each container runs a single `pensieve_server` process:

```
pensieve_server
  |
  +-- io_uring event loop (TCP data + UDP gossip)
  +-- Slab storage engine (64 MB per node by default)
  +-- SWIM gossip (discovers peers via seed node)
  +-- Coordinator (routes/proxies requests to key owner)
```

### Networking

- **Data plane (TCP 11211):** Client GET/PUT/DEL requests and peer-to-peer proxying.
- **Control plane (UDP 7946):** SWIM gossip heartbeats and membership updates.

All containers share a Docker bridge network with fixed IPs. Node1 acts as the bootstrap seed; other nodes join by pointing `PENSIEVE_SEED` at node1's gossip address.

### Configuration

Each node is configured via environment variables:

| Variable | Default | Description |
| :--- | :--- | :--- |
| `PENSIEVE_HOST` | `0.0.0.0` | Bind address and node identity |
| `PENSIEVE_DATA_PORT` | `11211` | TCP port for cache operations |
| `PENSIEVE_GOSSIP_PORT` | `7946` | UDP port for SWIM gossip |
| `PENSIEVE_MEMORY_MB` | `64` | Slab allocator size in MB |
| `PENSIEVE_SEED` | (empty) | Seed node as `host:gossip_port` |
| `PENSIEVE_SEED_DATA_PORT` | same as data_port | Seed's data port |

## Scaling

To add more nodes, copy any non-seed service block and change:

1. The container name and hostname
2. The `ipv4_address` (next available in the subnet)
3. The host port mapping

Example for a 6th node in the 5-node compose:

```yaml
  node6:
    <<: *pensieve-common
    container_name: pensieve-5n-node6
    hostname: node6
    networks:
      pensieve-net:
        ipv4_address: 172.31.0.16
    ports:
      - "11216:11211"
    environment:
      PENSIEVE_HOST: "172.31.0.16"
      PENSIEVE_DATA_PORT: "11211"
      PENSIEVE_GOSSIP_PORT: "7946"
      PENSIEVE_MEMORY_MB: "64"
      PENSIEVE_SEED: "172.31.0.11:7946"
    depends_on:
      - node1
```

## Stopping

```bash
# 3-node
docker compose -f deploy/docker-compose.3-node.yml down

# 5-node
docker compose -f deploy/docker-compose.5-node.yml down
```

Add `-v` to also remove volumes (none are used currently since storage is in-memory).

## Troubleshooting

### `io_uring_setup` fails

Your Docker host kernel must be 5.10+. Check with `uname -r`. If running Docker Desktop on macOS/Windows, the underlying Linux VM may have an older kernel.

### Container exits immediately

Check logs: `docker compose -f deploy/docker-compose.3-node.yml logs node1`

Common causes:
- Port already in use (change host port mappings)
- Insufficient `memlock` ulimit (the compose files set unlimited)

### Requests return ERROR

Allow a few seconds after startup for SWIM gossip to converge. If a key hashes to a node that hasn't joined the ring yet, the coordinator cannot proxy and returns ERROR.

## Client

The included `client.py` is a zero-dependency Python 3 script:

```bash
python3 deploy/client.py <host> <port> <GET|PUT|DEL> <key> [value]
```

It speaks the Pensieve binary wire protocol directly over TCP. See [Wire Protocol Reference](../docs/wire-protocol.md) for details on writing clients in other languages.
