# Plan — Distributed In-Memory Key-Value Store

A Redis-inspired in-memory key-value store in C++ with a custom TCP protocol,
an epoll-based multithreaded server, LRU eviction, and leader-follower replication.

Started May 2026. This document is the build plan; day-to-day state lives in [STATUS.md](STATUS.md).

---

## 1. Goals

| Goal | What "done" means |
|---|---|
| Correct core store | `GET/SET/DEL/EXPIRE/TTL/INCR` behave like their Redis counterparts for the supported types |
| Concurrency | Thousands of live connections on a handful of threads, no data races under TSan |
| Bounded memory | Hard `maxmemory` cap; LRU eviction reclaims before the allocator is exhausted |
| Fault tolerance | A follower can be promoted after leader loss with bounded data loss |
| Measurable | A benchmark harness that produces throughput/latency numbers we can quote |

Non-goals for v1: clustering/sharding, on-disk persistence beyond an append-only log,
Lua scripting, pub/sub, cluster-aware clients.

## 2. Architecture

```
            ┌──────────┐      ┌──────────┐
 clients ──▶│  acceptor│──────│ conn q   │
            └──────────┘      └────┬─────┘
                                   │ round-robin fd handoff
        ┌──────────────┬───────────┴───────────┬──────────────┐
        ▼              ▼                       ▼              ▼
   ┌─────────┐    ┌─────────┐             ┌─────────┐   ┌─────────┐
   │ worker 0│    │ worker 1│     ...     │ worker N│   │ replica │
   │ epoll   │    │ epoll   │             │ epoll   │   │ feeder  │
   └────┬────┘    └────┬────┘             └────┬────┘   └────┬────┘
        └──────────────┴──────────┬────────────┘             │
                                  ▼                          ▼
                       ┌────────────────────┐        ┌──────────────┐
                       │  sharded hash map  │        │ repl backlog │
                       │  + LRU clock       │        │  (ring buf)  │
                       └────────────────────┘        └──────────────┘
```

Key decisions:

- **One epoll loop per worker thread**, edge-triggered, non-blocking sockets. The
  acceptor thread hands accepted fds to workers round-robin. No thread-per-connection.
- **Sharded store**: the keyspace is split into `2^k` shards, each with its own mutex and
  LRU list. Shard index = `hash(key) & (nshards-1)`. This keeps lock contention off the
  hot path without a global lock or a single-threaded event loop.
- **Approximate LRU**, Redis-style: per-entry access timestamp, sample `N` keys on
  eviction and drop the oldest. Cheaper than maintaining an exact intrusive list under
  contention; revisit if hit-rate testing says otherwise.
- **Asynchronous replication**: the leader appends every mutating command to a ring
  backlog and streams it to followers. Followers are read-only. Failover is manual in v1
  (`REPLICAOF NO ONE`), automatic later.

## 3. Wire protocol

Length-prefixed binary, little-endian, one request per frame:

```
request :  [u32 nargs][u32 len][bytes] ... [u32 len][bytes]
response:  [u32 total][u8 type][payload]
           type = 0 NIL | 1 ERR | 2 STR | 3 INT | 4 ARR
```

Chosen over RESP text parsing because framing is trivial and the parser needs no
scanning. A `--resp` compatibility mode is a stretch goal so `redis-cli` can connect.

## 4. Milestones

### M1 — Single-threaded core *(complete)*
Blocking TCP server, protocol codec, hash map, `GET/SET/DEL`, a CLI client.

### M2 — epoll event loop *(complete)*
Non-blocking sockets, edge-triggered epoll, per-connection read/write buffers,
partial-frame handling, pipelining.

### M3 — Multithreading *(in progress)*
Acceptor + N workers, sharded map with per-shard locks, TSan-clean under a
concurrent stress client. Deliverable: `bench/stress.cpp`.

### M4 — TTL and LRU eviction
`EXPIRE`/`TTL`/`PERSIST`; lazy expiry on lookup plus an active expiry cycle sampling
random keys per tick. `maxmemory` + `maxmemory-policy` (`noeviction`, `allkeys-lru`).

### M5 — Replication
Leader command backlog, `REPLICAOF host port` handshake, full-sync snapshot followed
by an incremental stream, replica read-only enforcement, `INFO replication`.

### M6 — Benchmarks and hardening
Closed-loop benchmark harness: throughput vs. connection count, p50/p99/p999 latency,
hit rate under eviction, replication lag. ASan/TSan/UBSan in CI. Numbers land in
STATUS.md and the README.

### M7 — Packaging
CMake presets, Dockerfile, GitHub Actions matrix (gcc/clang, Debug/Release+sanitizers),
README with UML, a web page explaining the design.

## 5. Layout

```
src/     net/      socket, epoll loop, connection buffers
         proto/    frame codec, command parse/serialize
         store/    sharded map, entry, LRU, expiry
         repl/     backlog, leader feeder, follower client
         server/   config, worker pool, command dispatch
         cli/      interactive client
tests/             unit (doctest) + integration
bench/             stress client, latency harness
docs/              design notes, diagrams
```

## 6. Testing

- **Unit**: codec round-trips (including split frames), shard hashing distribution,
  LRU eviction order, TTL boundaries.
- **Integration**: spawn a server on an ephemeral port, drive it with the real client.
- **Concurrency**: TSan under mixed read/write load; a fuzz target feeding random bytes
  to the frame parser.
- **Replication**: kill the leader mid-stream, promote a follower, verify the keyspace
  diff is bounded by the unacked backlog.

## 7. Risks

| Risk | Mitigation |
|---|---|
| Lock contention on hot shards | Measure per-shard wait time; raise shard count, consider sharded-by-thread ownership |
| Edge-triggered epoll bugs (missed readiness) | Always drain to `EAGAIN`; integration test with slow/partial writers |
| Unbounded replication backlog | Fixed ring buffer; disconnect followers that fall behind, force a re-sync |
| Benchmark numbers that don't reproduce | Pin CPU affinity, report machine specs and methodology alongside results |

## 8. Deliverables for GitHub

- `README.md` with UML class + sequence diagrams (Mermaid, renders natively)
- `PLAN.md` (this file), `STATUS.md`
- CI badge, build instructions, benchmark methodology
- A published design page walking through the architecture
