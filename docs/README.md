# Design notes

This directory holds the design artifacts for the project: architecture notes, event-loop reasoning,
and the static design page used for project documentation.

Current baseline:

- leader/follower server layout with a worker-thread acceptor model
- binary protocol frame parser and command dispatcher
- sharded in-memory store with TTL, approximate LRU eviction, and `maxmemory` enforcement
- CLI and benchmark harness with CI and Docker packaging

Open the design page at [index.html](index.html) for an architecture walkthrough.
