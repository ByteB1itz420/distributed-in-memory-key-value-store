# Design notes

This directory holds the design artifacts for the project: architecture notes, event-loop reasoning,
and the static design page used for project documentation.

Current implementation:

- non-blocking TCP acceptor with per-worker edge-triggered epoll loops
- bounded binary request frames, partial-read handling, and pipelining
- thread-safe shared in-memory map with lazy TTL expiry and approximate memory accounting
- `noeviction` write rejection and least-recently-used `allkeys-lru` eviction
- interactive CLI, stress client, benchmark harness, CI, and Docker packaging
- bounded mutation-backlog groundwork; no follower replication or failover is implemented

Open the design page at [index.html](index.html) for an architecture walkthrough.
