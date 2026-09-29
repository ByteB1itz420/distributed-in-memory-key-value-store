# Benchmark harness

This directory contains the benchmark tooling for the M6 milestone.

Run the built harness against a live server:

```bash
./build/kvserver --port 6380 --threads 4 --maxmemory 512mb --maxmemory-policy allkeys-lru
./build/kvbench --host 127.0.0.1 --port 6380 --clients 8 --ops 1000 --keyspace 256 --value-size 64
```

The benchmark reports throughput and latency buckets for the target machine:

- total operations
- elapsed time
- ops/sec
- p50, p99, and p999 latency in microseconds

This is the baseline required before claiming throughput or latency numbers in the repo.
