# Benchmarks

All numbers on this page were measured with `aether_bench` and `aetherd` from this repository.
Reproduce them with:

```bash
cmake --preset release && cmake --build --preset release
./build/release/bench/aether_bench --count 5000000 --runs 5
./build/release/bench/aether_bench --count 5000000 --runs 5 --instruments 4
./build/release/apps/aetherd --count 5000000 --instruments 4
```

## Setup

| | |
|---|---|
| Machine | Apple M1 Pro, 10 cores, 32 GB |
| OS | macOS 27.0 |
| Compiler | Apple clang 21.0.0 |
| Flags | `-O3 -DNDEBUG -mcpu=native`, C++20 |
| Date | 2026-11-20 |

## Method

- The command stream is generated up front from a seeded RNG (seed 42), so the generator is
  never inside a timed loop. Mix: about 55% passive limit orders, 30% cancels, 10% aggressive
  orders (crossing limit, IOC, market) and 5% modifies. 5,000,000 commands produce about
  9.1 million output events.
- Each engine gets one untimed warm-up pass, then 5 timed passes on a fresh engine.
  **Orders/sec** is the median pass.
- **Latency** is a separate pass with `steady_clock` read before and after every command,
  recorded into a fixed log-scale histogram (no allocation while measuring).
- **Allocations** are counted by a replacement global `operator new`, inside the timed loop
  only. Memory the engine allocates at construction is not counted, on purpose.
- Both engines must produce the same number of events and the same checksum, or the
  benchmark exits with an error.

## Results: engine only (single thread)

1 instrument, 5,000,000 commands:

| Engine | Orders/sec | p50 | p99 | p99.9 | Heap allocations per order |
|---|---|---|---|---|---|
| Reference (`std::map` + `std::deque` + `std::unordered_map`) | 17.1 M | 42 ns | 335 ns | 591 ns | 0.551 (2,754,329 total) |
| Fast (pool + intrusive FIFO + flat price array + bitmap) | **57.6 M** | 41 ns | **85 ns** | **167 ns** | **0** (0 total) |

4 instruments, 5,000,000 commands:

| Engine | Orders/sec | p50 | p99 | p99.9 | Heap allocations per order |
|---|---|---|---|---|---|
| Reference | 13.3 M | 42 ns | 503 ns | 2,431 ns | 0.556 (2,779,204 total) |
| Fast | **51.1 M** | 41 ns | **125 ns** | **295 ns** | **0** (0 total) |

Fast vs reference: 3.4x throughput and 3.5x lower p99.9 on one instrument; 3.8x throughput
and 8.2x lower p99.9 on four instruments.

## Results: full threaded pipeline

`aetherd`: gateway/sequencer thread, engine thread and publisher thread connected by two
lock-free SPSC rings, 4 instruments, 5,000,000 commands.

| Mode | End-to-end throughput |
|---|---|
| Three threads, publisher hashing every event | 6.7 M commands/sec |
| Same, with every command journaled to disk first | 6.5 M commands/sec |

This is far below the engine-only number because the publisher computes a byte-wise FNV-1a
hash over every event and the threads yield when a ring is empty or full. The engine is not
the bottleneck in this pipeline.

## Determinism check

A 2,000,000-command, 4-instrument run was journaled by `aetherd` (96 MB log) and replayed
with `aether_replay`. All four runs gave the same output hash over 3,681,324 events:

| Run | Hash |
|---|---|
| Live, threaded, fast book | `0xb09b53edf57558ac` |
| Replay, single thread, fast book | `0xb09b53edf57558ac` |
| Replay, single thread, reference book | `0xb09b53edf57558ac` |
| Replay, threaded, fast book | `0xb09b53edf57558ac` |

## What these numbers do and do not mean

- **Clock resolution.** On Apple Silicon `steady_clock` advances in steps of about 41 ns
  (the benchmark prints the smallest step it sees). Per-command latencies are multiples of
  that step, so p50 = 41 ns means "at most one clock tick", not exactly 41 ns. The mean
  time per command from the throughput run is about 17 ns for the fast engine and 58 ns
  for the reference. The p99 and p99.9 figures are several ticks and are meaningful.
- **Synthetic flow.** The order flow is generated, not recorded from a real market. Real
  flow has different price and cancel patterns.
- **Engine only.** The latency numbers time `Engine::process` on one thread: no network,
  no decoding, no queueing.
- **Not a tuned machine.** macOS cannot pin threads to cores or isolate them, and nothing
  else was done to quiet the system. The `max` column is dominated by OS scheduling noise
  and is not reported here.
- **One machine, one compiler.** No Linux or x86 numbers yet.
