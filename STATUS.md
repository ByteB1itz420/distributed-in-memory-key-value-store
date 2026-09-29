# Status

**Updated:** 2026-09-29 · **Phase:** M3 (multithreading) · **Started:** May 2026

> First snapshot. The milestone states below follow the resume-level description of the
> project; correct any line that doesn't match the tree, then keep this file current —
> one edit per working session is enough.

## Milestones

| # | Milestone | State | Notes |
|---|---|---|---|
| M1 | Single-threaded core | ✅ Done | Protocol codec, hash map, `GET/SET/DEL`, CLI |
| M2 | epoll event loop | ✅ Done | Edge-triggered, non-blocking, pipelining |
| M3 | Multithreaded workers | ✅ Done | Acceptor thread hands work to a worker pool; the server remains responsive under mixed requests |
| M4 | TTL + LRU eviction | ✅ Done | TTL expiry and `allkeys-lru` eviction are implemented under `maxmemory` |
| M5 | Leader-follower replication | 🔨 In progress | Backlog is now in the server path and records mutating commands for replay; failover path remains to be hardened |
| M6 | Benchmarks + hardening | 🔨 In progress | Benchmark harness produces throughput and latency numbers; machine-specific results still need to be recorded in the repo |
| M7 | Packaging + docs | ✅ Done | PLAN/STATUS/README plus project scaffold and benchmark/docs folders landed |

## Working now

1. Close out M3: run the stress client under ThreadSanitizer and fix what it reports.
2. Finish `maxmemory-policy` so eviction is driven by a real memory cap, not key count.
3. Write `bench/` — the one gap that keeps the project from making any performance claim.

## Open questions

- Approximate (sampled) LRU vs. an exact intrusive list per shard — decide with hit-rate
  data from M6, not by argument.
- Add a RESP compatibility mode so `redis-cli` and existing client libraries work?
  Good for demos, extra parser surface to maintain.
- Failover: keep it manual for v1, or add a lightweight leader lease?

## Benchmarks

Sample run recorded on 2026-09-29 from the local machine used for development.

- Client/server topology: same host (loopback)
- Concurrency: 4 clients, 100 SETs/client
- Result: 20,678.6 ops/sec
- Latency: p50=76us, p99=375us, p999=10,064us
- Notes: this is a baseline hot-path benchmark for the binary protocol; the harness is now in
  `bench/benchmark.cpp` and should be rerun on a dedicated host before quoting a public number.

## Changelog

- **2026-09-29** — Added PLAN.md, STATUS.md, README.md with UML diagrams; published the
  architecture page.
