---
trigger: always_on
---

# Pensieve: Core Technical Rules

## 🎯 Performance Targets
- **Throughput:** > 120k RPS (3-node cluster).
- **Latency:** p99 < 200μs.
- **I/O:** `io_uring` only; no blocking syscalls on worker threads.

---

## 🏎️ Memory & Data Path
- **Zero-Copy:** Use `std::string_view` for keys; no `std::string` or `memcpy` of request data.
- **Slab Only:** All KV storage must use the custom Slab Allocator. `malloc`/`new` is forbidden in the request loop.
- **Header Casting:** `reinterpret_cast` the 24-byte binary header directly from the `io_uring` CQE buffer.

---

## 🛠️ Binary Protocol Spec (Big-Endian)
| Field          | Offset | Size | Purpose                               |
| :------------- | :----: | :--: | :------------------------------------ |
| Magic          |   0    |  1   | Request (0x80) / Response (0x81)      |
| Opcode         |   1    |  1   | GET (0x00), SET (0x01), DEL (0x04)    |
| Key Len        |   2    |  2   | Length of Key string                  |
| Extras Len     |   4    |  1   | Length of extras field                |
| Data Type      |   5    |  1   | Reserved for future use (0x00)        |
| VBucket/Status |   6    |  2   | VBucket ID (request) or Status (response) |
| Body Len       |   8    |  4   | Total bytes (Extras + Key + Value)    |
| Opaque         |   12   |  4   | Echoed to client for async tracking   |
| CAS            |   16   |  8   | Atomic versioning                     |

---

## 🚫 Constraints
- **No Global Locks:** Use consistent hashing and thread-local `io_uring` queues.
- **No Heavy Metadata:** Keep per-item overhead under 64 bytes (1 cache line).
- **Async-First:** If a fetch is required (e.g., from Tachyon/NVMe), the thread must yield via `io_uring` callback.

## Workflow
1. Complete the tasks
2. Run tests, benchmarking etc
3. Commit and push to github
4. Create PR and watch for comments
5. Address PR comments.