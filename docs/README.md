# Design notes

This directory is reserved for architecture notes, event-loop reasoning, and failure-mode documentation.

Current baseline:

- single-node server with binary protocol
- shared in-memory store with TTL and LRU-style eviction
- CLI smoke tests and CTest coverage
