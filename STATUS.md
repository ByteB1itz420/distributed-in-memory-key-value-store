# Status

**Updated:** 2026-09-29 · **Phase:** M7 (packaging + docs) · **Started:** May 2026

> Current implementation snapshot. Milestone 7 packaging and docs are present, but M5
> replication and M6 hardening are still in progress.

## Milestones

| # | Milestone | State | Notes |
|---|---|---|---|
| M1 | Single-threaded core | ✅ Done | Protocol codec, hash map, `GET/SET/DEL`, CLI |
| M2 | epoll event loop | ✅ Done | Edge-triggered, non-blocking, pipelining |
| M3 | Multithreaded workers | 🔨 In progress | Acceptor hands non-blocking sockets to per-worker epoll loops; store access still uses one global mutex and TSan validation remains |
| M4 | TTL + LRU eviction | ✅ Done | Lazy TTL pruning; approximate memory accounting supports `noeviction` rejection and `allkeys-lru` eviction |
| M5 | Leader-follower replication | 🔨 In progress | Recent mutations are retained in a bounded in-process backlog; follower sync, read-only mode, and failover are not implemented |
| M6 | Benchmarks + hardening | 🔨 In progress | Stress and benchmark tools are present; sanitizer, load, and reproducible benchmark validation remain |
| M7 | Packaging + docs | ✅ Done | PLAN/STATUS/README plus project scaffold and benchmark/docs folders landed |

## Working now

1. Run the stress client under ThreadSanitizer and fix any reported races.
2. Measure the store-wide mutex and LRU scan before deciding whether to shard or optimize eviction.
3. Implement and test follower synchronization, read-only behavior, and manual promotion before claiming replication support.
4. Re-run benchmarks on a documented machine and report the methodology with the results.

## Open questions

- Retain the current exact LRU scan or optimize it — decide using M6 hit-rate and latency data.
- Add a RESP compatibility mode so `redis-cli` and existing client libraries work?
  Good for demos, extra parser surface to maintain.
- Failover: retain manual promotion for v1, or add a lightweight leader lease?

## Benchmarks

Historical sample recorded on 2026-09-29 before the audit changes; re-run before quoting it.

- Client/server topology: same host (loopback)
- Concurrency: 4 clients, 100 SETs/client
- Result: 20,678.6 ops/sec
- Latency: p50=76us, p99=375us, p999=10,064us
- Notes: same-host loopback result from the then-current code, not a production claim. The
  harness is in `bench/benchmark.cpp`; rerun it with system details before publishing.

## Changelog

- **2026-09-29** — Added PLAN.md, STATUS.md, README.md with UML diagrams; published the
  architecture page.
