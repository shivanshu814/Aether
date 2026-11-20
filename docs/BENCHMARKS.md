# Benchmarks

All numbers on this page were measured with `aether_bench` and `aetherd` from this repository.
Reproduce them with:

```bash
cmake --preset release && cmake --build --preset release
./build/release/bench/aether_bench --count 5000000 --runs 5
./build/release/bench/aether_bench --count 5000000 --runs 5 --instruments 4
./build/release/apps/aetherd --count 5000000 --instruments 4
./build/release/apps/aetherd --count 5000000 --instruments 4 --log run.log
./build/release/bench/aether_bench --count 5000000 --runs 5 --shards 8
```

## Setup

| | |
|---|---|
| Machine | Apple M1 Pro (8 performance + 2 efficiency cores), 32 GB |
| OS | macOS 27.0 |
| Compiler | Apple clang 21.0.0 |
| Flags | `-O3 -DNDEBUG -mcpu=native`, C++20 |
| Date | 2026-11-20 |

## The workload

The command stream is generated up front from a seeded RNG (seed 42), so the generator is
never inside a timed loop. The generator runs a reference engine alongside itself, so it
always knows which orders are resting: every cancel and modify names an order that is in
the book at that moment.

What the 5,000,000-command, one-instrument stream actually contains (measured, not the
generator's nominal percentages):

| | |
|---|---|
| New orders | 62.0% of commands. 20.1% of them trade on arrival; 4.0% are market orders, 4.0% IOC |
| Cancels | 33.0%, every one removes a resting order |
| Modifies | 5.0% |
| Rejected commands | 0 |
| Resting orders | the book holds about 20,000 once it has filled |
| Output events | 9,125,791 (3.22 M acks, 1.39 M trades, 2.59 M rested, 1.77 M canceled, 0.13 M modified, 0.03 M expired) |

## Method

- Each engine gets one untimed warm-up pass, then timed passes on a fresh engine.
  **Commands/sec** is the median pass. Commands go through `Engine::process_batch`, which
  prefetches the id-index slot 12 commands ahead and the order 4 commands ahead.
- **Latency** is a separate pass with `steady_clock` read before and after every command,
  recorded into a fixed log-scale histogram (no allocation while measuring).
- **Allocations** are counted by a replacement global `operator new`, inside the timed loop
  only. Memory the engine allocates at construction is not counted, on purpose.
- Both engines must produce the same number of events and the same checksum, or the
  benchmark exits with an error.

## Results: engine only (single thread)

1 instrument, 5,000,000 commands. Three separate benchmark runs (62.5, 63.7 and 62.9 M
commands/sec); the latency figures were the same in all three.

| Engine | Commands/sec | p50 | p99 | p99.9 | Heap allocations per command |
|---|---|---|---|---|---|
| Reference (`std::map` + `std::deque` + `std::unordered_map`) | 4.2 M | 125 ns | 1,007 to 1,087 ns | 3,007 to 4,031 ns | 0.576 (2,880,277 total) |
| Fast (pool + intrusive FIFO + flat price array + bitmap) | **63 M** | 41 ns | **83 ns** | **167 ns** | **0** (0 total) |

4 instruments, 5,000,000 commands, median of 5 benchmark runs:

| Engine | Commands/sec | p99 | p99.9 (median) | p99.9 (range over 5 runs) | Heap allocations per command |
|---|---|---|---|---|---|
| Reference | 3.8 M | 1,215 ns | 4,479 ns | 4,095 to 4,863 ns | 0.581 |
| Fast | **57.6 M** | **125 ns** | **251 ns** | 211 to 335 ns | **0** |

Fast vs reference: 15x the throughput, 13x lower p99 and about 20x lower p99.9 on one
instrument; 15x the throughput, 10x lower p99 and 18x lower median p99.9 on four.

**Read this comparison for what it is.** The reference book is the test oracle: it is
written to be obviously correct, not fast. Its cancel does a linear scan of the queue at
that price, and with 20,000 resting orders spread over about 20 price levels a queue holds
around 1,000 orders. That scan is most of the gap. The comparison shows what the data
structures buy over the simplest correct implementation; it is not a comparison against
another optimised engine.

## Results: full threaded pipeline

`aetherd`: gateway/sequencer thread, engine thread and publisher thread connected by two
lock-free SPSC rings, 4 instruments, 5,000,000 commands, median of 3 runs. The publisher
hashes every output event.

| Mode | End-to-end throughput |
|---|---|
| Three threads | 27.6 M commands/sec |
| Three threads, every command journaled to disk before the engine sees it | 15.5 M commands/sec |

## Results: scaling across cores (sharding)

`aether_bench --shards 8`: one engine thread per shard, each with its own instrument and its
own seeded stream of 5,000,000 commands, all released together. No locks and no shared
state between shards. Each figure is the median of 5 passes; two separate runs are shown.
Shards call `process()` per command, without the lookahead prefetch: with 8 threads the
prefetches cost more than they save (about 240 M against 270 M aggregate, measured).

Two ways of adding the threads up, because they differ:

- **Until last shard:** all commands divided by the wall time until the slowest thread
  finishes. This is the conservative figure and the one quoted elsewhere.
- **Sum of shard rates:** what each thread sustained on its own stream, added together.

| Engine threads | Until last shard (run 1 / run 2) | Sum of shard rates | Slowest shard | Fastest shard | Heap allocations |
|---|---|---|---|---|---|
| 1 | 61.6 M / 62.9 M | 61.6 M | 61.6 M | 61.6 M | 0 |
| 2 | 96.9 M / 105.6 M | 103.5 M | 48.4 M | 55.1 M | 0 |
| 4 | 152.5 M / 145.6 M | 173.4 M | 38.1 M | 52.2 M | 0 |
| 6 | **216.2 M / 213.2 M** | 252.3 M | 36.0 M | 49.3 M | 0 |
| 8 | **285.7 M / 255.8 M** | 331.7 M | 35.7 M | 49.7 M | 0 |

Across five separate runs on this page's history the 8-thread figure has ranged from 256 M
to 286 M; "about 270 M" is the fair summary.

### Why it does not scale linearly

At 8 threads a shard runs at 34 to 51 M commands/sec instead of 60 M. Measurements:

| Test | Result | What it says |
|---|---|---|
| Book size: 20,000 against 200 resting orders | 1 thread: 54 M against 71 M. 8 threads, per thread: 33 M against 38 M | Cache matters: a deep book costs about 25%. But a tiny book still loses almost half its speed at 8 threads, so cache is not the main cause |
| 8 threads only reading their command arrays, no engine | 2,281 M commands/sec, about 109 GB/s | Not memory bandwidth: the engines read about 13 GB/s |
| Per-thread rates within one pass | fastest 51 M, slowest 34 M | The threads are not slowed evenly; some get less CPU |
| Raising the thread priority class | no change | |

The uneven per-thread speed points at scheduling: macOS decides which thread runs on which
core (performance or efficiency) and cannot be told to pin them. With every thread on a
performance core at single-thread speed the ceiling on this machine would be about
8 x 60 M = 480 M commands/sec.

### Linux with thread pinning (in a VM on the same laptop)

To test that, the benchmark was built and run inside a Linux virtual machine on the same
M1 Pro: OrbStack, kernel 6.17, Debian 12, g++ 12.2, `-O3 -mcpu=native`, 10 virtual CPUs.
`--pin 1` binds shard `s` to CPU `s` with `pthread_setaffinity_np`. One run each.

| Engine threads | macOS (earlier run) | Linux VM, not pinned | Linux VM, pinned |
|---|---|---|---|
| 1 | 62.3 M | 54.4 M | 56.8 M |
| 2 | 96.7 M | 88.6 M | 71.9 M |
| 4 | 152.2 M | 142.8 M | 145.0 M |
| 6 | 214.2 M | 214.7 M | 217.1 M |
| 8 | 271.3 M | 251.5 M | 267.8 M |

The VM is no faster than macOS and noisier (two pinned threads managed only 71.9 M in this
run). Pinning made a small difference at 7 and 8 threads and none below. This does not
settle the question: the VM's CPUs are virtual, so pinning binds a thread to a virtual CPU
while macOS underneath still chooses the physical core. A bare-metal Linux machine with
isolated cores has not been measured.

What sharding does and does not give:

- Each instrument still sees its commands in their original order, so every shard's output
  is deterministic. A test checks each shard's hash against a single-threaded run of the
  same shard, with both books.
- There is no defined order between events of different shards.
- It needs at least as many busy instruments as threads. One very active instrument is
  still limited to one thread, about 60 M commands/sec here.

## Determinism check

A 2,000,000-command, 4-instrument run was journaled by `aetherd` (96 MB log) and replayed
with `aether_replay`. All four runs gave the same output hash over 3,661,486 events:

| Run | Hash |
|---|---|
| Live, threaded, fast book | `0xddd9529a77594a11` |
| Replay, single thread, fast book | `0xddd9529a77594a11` |
| Replay, single thread, reference book | `0xddd9529a77594a11` |
| Replay, threaded, fast book | `0xddd9529a77594a11` |

## Where a command's time goes

Single thread, fast engine, to see how much room is left:

| Measurement | Time per command |
|---|---|
| Only reading the command array | 0.9 ns |
| Reading plus an unpredictable branch on command type and side, no other work | 3.5 ns |
| Burst of passive inserts only, 20,000 resting orders | 7.1 ns |
| Burst of cancels only, random resting order | 7.1 ns |
| Inserts and cancels in runs of 64 of each, no trades | 7.4 ns |
| The same inserts and cancels randomly interleaved | 10.3 ns |
| Full benchmark mix (trades, modifies, market and IOC orders), no prefetch | 16.2 ns |
| Full benchmark mix with the lookahead prefetch (`process_batch`) | 15.9 ns |

Random order flow makes the command type unpredictable, and that alone costs about 3 ns per
command (10.3 against 7.4). Matching, modifies and the crossing test account for the rest.
The straight-line work is about 7 ns, so the engine is roughly a factor of two above what
this design could reach if nothing were ever mispredicted.

## Optimisation log

Every change was measured before and after on the same machine. Rows marked (old stream)
were measured before the workload fix below and are kept as measured.

| Change | Before | After | Kept |
|---|---|---|---|
| Reference book replaced by pool + intrusive FIFO + flat price arrays + bitmap (old stream) | 17.2 M/s, 0.551 allocs | 57.6 M/s, 0 allocs | yes |
| Pipeline: batched ring transfers (one atomic update per batch of up to 256), spin-then-yield waiting, one hash multiply per field instead of per byte (old stream) | 6.7 M/s | 25.4 M/s | yes |
| Book: per-side state in an array indexed by side, branch-free crossing test (old stream) | 57.6 M/s | 60.5 M/s | yes |
| Sharding: one engine thread per group of instruments, no shared state | 60 M/s on 1 thread | about 270 M/s on 8 threads | yes |
| **Workload fix.** The generator did not know when orders were filled, so 90% of its cancels and modifies named orders that were already gone and were rejected almost for free, and the book held about 2,000 orders instead of 20,000. It now tracks resting orders through a reference engine | fast 60.5 M/s, reference 17.2 M/s | fast 61.9 M/s, reference 4.2 M/s | yes |
| Pool: stack of free pointers instead of a free list threaded through the slots, which made each allocation wait on a load from the previous slot | insert burst 11.5 ns | insert burst 7.1 ns; full mix unchanged | yes |
| Price level: sentinel ring, so link and unlink have no branches; branch-free bitmap clear | 60.2 M/s | 59.7 to 61.7 M/s (no measurable change) | yes: simpler code, no slower |
| Lookahead prefetch, second attempt: `process_batch` prefetches the id-index slot 12 commands ahead and the order 4 ahead, with no branches in the hint (the first attempt branched on the lookahead command and was much slower) | 1 instrument 61.9 M/s; 4 instruments 45.2 M/s; pipeline 22.6 M/s | 63 M/s; 57.6 M/s; 27.6 M/s | yes, except in the sharded runner, where 8 threads of prefetches made it slower (240 M against 270 M) |
| Compiler flags: `-fomit-frame-pointer`, `-fno-exceptions -fno-rtti`, `-O2` | 61 M/s | 59 to 61 M/s | no: no difference |
| Profile-guided optimisation (clang `-fprofile-instr-use`) (old stream) | 57.5 M/s | 59.2 M/s | no: about 3%, not worth a two-stage build |
| Software prefetch, first attempt: hints that branched on the type of the lookahead command (old stream) | 57.9 M/s | 34.3 M/s | no: much slower. The branches in the hint cost more than the misses they hid |

Two things in earlier versions of this page were wrong and have been corrected: the
four-instrument p99.9 comparison came from a single noisy run, and the scaling loss was
blamed on cache and memory bandwidth before it had been measured.

## What these numbers do and do not mean

- **Clock resolution.** On Apple Silicon `steady_clock` advances in steps of about 41 ns
  (the benchmark prints the smallest step it sees). Per-command latencies are multiples of
  that step, so p50 = 41 ns means "at most one clock tick", not exactly 41 ns. The mean
  time per command from the throughput run is about 16 ns for the fast engine and 240 ns
  for the reference. The p99 and p99.9 figures are several ticks and are meaningful.
- **Synthetic flow.** The order flow is generated, not recorded from a real market. Real
  flow has different price and cancel patterns.
- **Engine only.** The latency numbers time `Engine::process` on one thread: no network,
  no decoding, no queueing. The pipeline numbers are throughput only, not latency.
- **Not a tuned machine.** macOS cannot pin threads to cores or isolate them, and nothing
  else was done to quiet the system. The `max` column is dominated by OS scheduling noise
  and is not reported here.
- **One machine.** Everything ran on one Apple M1 Pro laptop. The only Linux numbers are
  from a VM on that laptop; there are no bare-metal Linux or x86 numbers.
