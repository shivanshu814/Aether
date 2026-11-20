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

## Results: scaling across cores (sharding)

`aether_bench --shards 8`: one engine thread per shard, each with its own instrument and its
own seeded stream of 5,000,000 commands, all released together. No locks and no shared
state between shards. Median of 5 passes.

Two ways of adding the threads up are shown, because they differ:

- **Until last shard:** all commands divided by the wall time until the slowest thread
  finishes. This is the conservative figure and the one quoted elsewhere.
- **Sum of shard rates:** what each thread sustained on its own stream, added together.

| Engine threads | Until last shard | Sum of shard rates | Slowest shard | Fastest shard | Heap allocations |
|---|---|---|---|---|---|
| 1 | 59.5 M | 59.5 M | 59.5 M | 59.5 M | 0 |
| 2 | 103.8 M | 111.1 M | 51.9 M | 59.2 M | 0 |
| 4 | 161.3 M | 181.0 M | 40.3 M | 53.7 M | 0 |
| 6 | **227.7 M** | 279.2 M | 38.0 M | 54.9 M | 0 |
| 8 | **280.0 M** | 379.1 M | 35.0 M | 55.8 M | 0 |

Across three separate benchmark runs the 8-thread "until last shard" figure was 263 M,
275 M and 280 M commands/sec, and the 6-thread figure 211 M, 230 M and 228 M.

### Why it does not scale linearly

Even with 8 threads the fastest shard still runs at 54 to 56 M commands/sec, the same as a
lone engine, while the slowest drops to about 35 M. So the engines do not slow each other
down evenly; some threads are simply given less CPU. Three measurements narrow this down:

| Test | Result | What it rules out |
|---|---|---|
| Shrinking the resting-order working set 100x (20,000 to 200 live orders) | no change in per-thread speed, at 1, 4 or 8 threads | cache capacity |
| 8 threads only reading their command arrays, no engine | 2,267 M commands/sec, about 109 GB/s | memory bandwidth: the engines read about 13 GB/s |
| Per-thread rates within one pass | 33 to 56 M at 8 threads; at 4 threads one pass had all four at 54 to 58 M and the next had 38 to 57 M | a fixed hardware limit |

What is left is scheduling. The machine has 8 performance and 2 efficiency cores, macOS
decides which thread runs where and cannot be told to pin them, and raising the thread
priority class made no difference. With every thread on a performance core at single-thread
speed the ceiling on this machine would be about 8 x 58 M = 460 M commands/sec. Reaching it
needs an OS that can pin threads to isolated physical cores. Bare-metal Linux has not been
measured; a Linux VM on this laptop is below.

### Linux with thread pinning (in a VM on the same laptop)

To test the scheduling explanation, the same benchmark was built and run inside a Linux
virtual machine on the same M1 Pro: OrbStack, kernel 6.17, Debian 12, g++ 12.2,
`-O3 -mcpu=native`, 10 virtual CPUs. `--pin 1` binds shard `s` to CPU `s` with
`pthread_setaffinity_np`.

```bash
./aether_bench --count 5000000 --runs 5 --shards 8           # not pinned
./aether_bench --count 5000000 --runs 5 --shards 8 --pin 1   # pinned
```

| Engine threads | macOS, until last shard | Linux VM, not pinned | Linux VM, pinned | Linux VM pinned, sum of shard rates |
|---|---|---|---|---|
| 1 | 59.5 M | 61.7 M | 61.4 M | 61.4 M |
| 2 | 103.8 M | 122.5 M | 121.0 M | 121.6 M |
| 4 | 161.3 M | 196.8 M | 188.6 M | 213.0 M |
| 6 | 227.7 M | 262.8 M | 255.8 M | 269.6 M |
| 8 | 280.0 M | 306.8 M | **329.7 M** | 435.8 M |

Single-thread results in the VM (g++): fast engine 62.1 M commands/sec, p99 125 ns,
p99.9 211 ns, 0 allocations; reference 17.3 M.

What this shows, and what it does not:

- Linux gave 10 to 18% more aggregate throughput than macOS at 8 threads, and two threads
  scaled almost perfectly (122 M against 104 M). That supports scheduling as the cause.
- Pinning helped at 7 and 8 threads (330 M against 307 M) and made no difference below that.
- It is still well short of the 460 to 490 M that 8 full-speed cores would give. The VM's
  CPUs are virtual: pinning binds a thread to a virtual CPU, and macOS underneath still
  decides which physical core, performance or efficiency, runs it. This is not a
  bare-metal Linux result.
- Each configuration was run once (median of 5 passes inside the run).

What sharding does and does not give:

- Each instrument still sees its commands in their original order, so every shard's output
  is deterministic. A test checks each shard's hash against a single-threaded run of the
  same shard, with both books.
- There is no defined order between events of different shards.
- It needs at least as many busy instruments as threads. One very active instrument is
  still limited to one thread, about 60 M commands/sec here.

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
| Sharding: one engine thread per group of instruments, no shared state | 60 M/s on 1 thread | 263 to 280 M/s on 8 threads | yes |
| Linux VM with threads pinned to (virtual) CPUs, 8 engine threads | 280 M/s on macOS | 330 M/s | n/a: environment, not code |
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
- **One machine.** Everything ran on one Apple M1 Pro laptop. The only Linux numbers are
  from a VM on that laptop; there are no bare-metal Linux or x86 numbers.
