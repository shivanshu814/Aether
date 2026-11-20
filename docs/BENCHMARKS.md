# Benchmarks

All numbers on this page were measured with `aether_bench` and `aetherd` from this repository.
Reproduce them with:

```bash
cmake --preset release && cmake --build --preset release
./build/release/bench/aether_bench --count 5000000 --runs 5
./build/release/bench/aether_bench --count 5000000 --runs 5 --instruments 4
./build/release/apps/aetherd --count 5000000 --instruments 4
./build/release/apps/aetherd --count 5000000 --instruments 4 --log run.log
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
- Each engine gets one untimed warm-up pass, then timed passes on a fresh engine.
  **Commands/sec** is the median pass.
- **Latency** is a separate pass with `steady_clock` read before and after every command,
  recorded into a fixed log-scale histogram (no allocation while measuring).
- **Allocations** are counted by a replacement global `operator new`, inside the timed loop
  only. Memory the engine allocates at construction is not counted, on purpose.
- Both engines must produce the same number of events and the same checksum, or the
  benchmark exits with an error.

## Results: engine only (single thread)

1 instrument, 5,000,000 commands. These figures were the same across repeated runs.

| Engine | Commands/sec | p50 | p99 | p99.9 | Heap allocations per command |
|---|---|---|---|---|---|
| Reference (`std::map` + `std::deque` + `std::unordered_map`) | 17.2 M | 42 ns | 335 ns | 591 ns | 0.551 (2,754,329 total) |
| Fast (pool + intrusive FIFO + flat price array + bitmap) | **60.5 M** | 41 ns | **85 ns** | **167 ns** | **0** (0 total) |

4 instruments, 5,000,000 commands, median of 5 benchmark runs:

| Engine | Commands/sec | p99 | p99.9 (median) | p99.9 (range over 5 runs) | Heap allocations per command |
|---|---|---|---|---|---|
| Reference | 13.3 M | 463 ns | 879 ns | 799 to 2,623 ns | 0.556 |
| Fast | **51.9 M** | **125 ns** | **251 ns** | 251 to 423 ns | **0** |

Fast vs reference: 3.5x the throughput and 3.5x lower p99.9 on one instrument; 3.9x the
throughput, 3.7x lower p99 and 3.5x lower median p99.9 on four instruments. The
four-instrument p99.9 moves between runs on this untuned machine, so the range is shown;
a single run can make the gap look much larger or smaller than the median.

## Results: full threaded pipeline

`aetherd`: gateway/sequencer thread, engine thread and publisher thread connected by two
lock-free SPSC rings, 4 instruments, 5,000,000 commands, median of 3 runs. The publisher
hashes every output event.

| Mode | End-to-end throughput |
|---|---|
| Three threads | 25.4 M commands/sec |
| Three threads, every command journaled to disk before the engine sees it | 15.4 M commands/sec |

## Determinism check

A 2,000,000-command, 4-instrument run was journaled by `aetherd` (96 MB log) and replayed
with `aether_replay`. All four runs gave the same output hash over 3,681,324 events:

| Run | Hash |
|---|---|
| Live, threaded, fast book | `0x56ece52b331d50b3` |
| Replay, single thread, fast book | `0x56ece52b331d50b3` |
| Replay, single thread, reference book | `0x56ece52b331d50b3` |
| Replay, threaded, fast book | `0x56ece52b331d50b3` |

## Optimisation log

Every change was measured before and after on the same machine and stream.

| Change | Before | After | Kept |
|---|---|---|---|
| Reference book replaced by pool + intrusive FIFO + flat price arrays + bitmap | 17.2 M/s, 0.551 allocs | 57.6 M/s, 0 allocs | yes |
| Pipeline: batched ring transfers (one atomic update per batch of up to 256), spin-then-yield waiting, and one hash multiply per field instead of per byte | 6.7 M/s | 25.4 M/s | yes |
| Book: per-side state in an array indexed by side and a branch-free crossing test, so resting and cancelling do not branch on buy/sell | 57.6 M/s | 60.5 M/s | yes |
| Profile-guided optimisation (clang `-fprofile-instr-use`) | 57.5 M/s | 59.2 M/s | no: about 3%, not worth a two-stage build |
| Software prefetch of the id index and the order a few commands ahead | 57.9 M/s | 34.3 M/s | no: slower. The working set already fits in cache; the cost is branches, not memory |

## What these numbers do and do not mean

- **Clock resolution.** On Apple Silicon `steady_clock` advances in steps of about 41 ns
  (the benchmark prints the smallest step it sees). Per-command latencies are multiples of
  that step, so p50 = 41 ns means "at most one clock tick", not exactly 41 ns. The mean
  time per command from the throughput run is about 16.5 ns for the fast engine and 58 ns
  for the reference. The p99 and p99.9 figures are several ticks and are meaningful.
- **Synthetic flow.** The order flow is generated, not recorded from a real market. Real
  flow has different price and cancel patterns.
- **Engine only.** The latency numbers time `Engine::process` on one thread: no network,
  no decoding, no queueing. The pipeline numbers are throughput only, not latency.
- **Not a tuned machine.** macOS cannot pin threads to cores or isolate them, and nothing
  else was done to quiet the system. The `max` column is dominated by OS scheduling noise
  and is not reported here.
- **One machine, one compiler.** No Linux or x86 numbers yet.
