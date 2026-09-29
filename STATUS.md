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
| M5 | Leader-follower replication | ⬜ Not started | Backlog and stream design are documented; failover path is not implemented yet |
| M6 | Benchmarks + hardening | 🔨 In progress | Stress harness is added; throughput and latency numbers still need to be recorded |
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

None recorded yet. Nothing in this repo or its docs should quote a throughput or latency
figure until `bench/` produces one on a described machine. When results land, record
here: ops/sec vs. connection count, p50/p99/p999, hit rate under eviction, replication
lag, plus CPU model, core count, and whether client and server shared a host.

## Changelog

- **2026-09-29** — Added PLAN.md, STATUS.md, README.md with UML diagrams; published the
  architecture page.
