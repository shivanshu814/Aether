# Aether

Ultra-low-latency matching engine in C++20: price-time priority order book, zero-allocation
hot path, lock-free SPSC rings and deterministic event replay.

Aether is a miniature electronic exchange. Orders come in and are given a global sequence
number. A single-threaded engine matches them by price-time priority, with one order book per
instrument, and emits acks, trades and cancels. The engine is a pure function of its input
stream, so any run can be replayed from the log and checked hash-for-hash.

## Results

Measured on an Apple M1 Pro with Apple clang 21 (`-O3 -mcpu=native`), 5,000,000 commands,
one instrument, engine only on a single thread:

| Engine | Commands/sec | p99 | p99.9 | Heap allocations per command |
|---|---|---|---|---|
| Reference (`std::map`) | 4.2 M | about 1,050 ns | about 3,700 ns | 0.576 |
| Fast (pool + flat array) | **63 M** | **83 ns** | **167 ns** | **0** |

The fast engine has 15x the throughput and about 20x lower p99.9, and makes zero heap
allocations after start-up. Both engines produce identical output, event for event. The
reference is the test oracle, written to be obviously correct rather than fast (its cancel
scans the queue at a price), so this is the gain over the simplest correct book, not over
another optimised engine.

The stream is 62% new orders, 33% cancels and 5% modifies against a book of about 20,000
resting orders; every cancel and modify hits a real order.

The full pipeline (gateway, engine and publisher threads joined by lock-free rings) runs at
27.6 M commands/sec, or 15.5 M with every command journaled to disk first.

Sharded by instrument across engine threads, with no locks and no shared state, aggregate
throughput passes 200 M commands/sec at 6 threads and reaches about 270 M at 8 threads.

Method, the four-instrument numbers, the optimisation log and the limits of these
measurements (clock resolution, synthetic order flow, untuned macOS) are in
[docs/BENCHMARKS.md](docs/BENCHMARKS.md).

## What is in it

- **Order types:** Limit, Market and IOC, plus cancel and modify. Reducing quantity keeps
  time priority; changing price or increasing quantity loses it.
- **Lookahead prefetch.** `Engine::process_batch` prefetches the id-index slot and the order of
  commands a few places ahead, with no branches in the hint, so a cancel's dependent cache
  misses are in flight before the command is reached.
- **Two order books with the same behaviour.** `RefOrderBook` is a simple `std::map` book
  kept as the test oracle. `OrderBook` is the fast one: tick-indexed price arrays, an
  intrusive FIFO ring per price level, a bitmap to find the next non-empty level, an
  order-id index and a fixed object pool.
- **Deterministic replay.** Every sequenced command is journaled as a 48-byte record.
  `aether_replay` re-runs a log into a fresh engine and prints an FNV-1a hash of the output.
- **Threaded pipeline.** Gateway/sequencer, engine and publisher threads joined by lock-free
  single-producer single-consumer ring buffers, with batched hand-off. Only the engine thread
  touches the books.
- **Benchmark harness.** Seeded load generator that tracks which orders are resting,
  log-scale latency histogram and a counting `operator new`.

## How it is tested

89 tests, run in three builds: Debug with AddressSanitizer + UndefinedBehaviorSanitizer,
Release, and Debug with ThreadSanitizer.

- **Scenario tests** run every hand-written case against both books, including the design
  document's walkthrough (Buy 450 @ 101.02 fills exactly five makers).
- **Differential tests** feed the same seeded random stream to the reference and the fast
  engine and compare their events after every command, then compare the full book depth.
  A second configuration uses a tiny book to force price-band and book-full rejects.
- **Replay tests** check that a journaled run replays to the same hash, that the threaded
  pipeline matches the single-threaded engine, and pin a golden hash for a fixed stream.

## Build and run

Requirements: CMake 3.25+, Ninja and a C++20 compiler. GoogleTest is downloaded by CMake.

```bash
brew install cmake ninja   # macOS

# Debug: -O0 -g with AddressSanitizer + UndefinedBehaviorSanitizer
cmake --preset debug && cmake --build --preset debug && ctest --preset debug

# Release: -O3 -mcpu=native, also builds the benchmark
cmake --preset release && cmake --build --preset release && ctest --preset release

# ThreadSanitizer
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

```bash
# Benchmark both engines
./build/release/bench/aether_bench --count 5000000

# Run the threaded exchange on generated flow and journal it
./build/release/apps/aetherd --count 2000000 --instruments 4 --log run.log

# Replay the journal; the hash must match the one aetherd printed
./build/release/apps/aether_replay run.log
./build/release/apps/aether_replay run.log --reference
```

## Project layout

```
aether/
├── libs/                 one small library per folder; each sees only what it links
│   ├── core/             types, Command / Event, EventSink concept, BookConfig
│   ├── memory/           Pool<T>: fixed-capacity object pool
│   ├── concurrency/      SpscRing<T>: lock-free single-producer single-consumer queue
│   ├── reference/        RefOrderBook: slow std::map oracle
│   ├── book/             OrderBook: the fast book (Order, PriceLevel, LevelBitmap)
│   ├── engine/           Engine<Book>: instrument id -> book, command dispatch
│   ├── journal/          event log reader/writer, FNV-1a hash sink
│   ├── sim/              deterministic load generator
│   └── runtime/          threaded gateway -> engine -> publisher pipeline, engine sharding
├── apps/
│   ├── aetherd/          the threaded exchange on generated flow
│   └── replay/           replay a log, print the output hash
├── bench/                benchmark, latency histogram, allocation counter
├── tests/                unit, scenario, differential and replay tests
└── docs/                 design document and benchmark results
```

## Design

The full design, with diagrams and pseudocode, is in
[docs/AETHER_DESIGN.md](docs/AETHER_DESIGN.md).

Key rules:

- Prices are integer ticks, never floating point.
- The engine never reads the clock, the network or the disk. It only sees sequenced commands.
- All memory is allocated at start-up. The hot path never calls `new`.
- `Command` and `Event` are fixed 48-byte, trivially copyable structs, enforced by
  `static_assert`. `Order` is exactly one 64-byte cache line.
- Events leave the engine through a C++20 concept, not a virtual call.

## Not done yet

- Bare-metal Linux and x86 benchmarks with isolated cores. A Linux VM on the same laptop
  was measured and was no faster than macOS.
- A network gateway and a binary wire protocol. Order flow is generated in-process.
- Sharding inside the live pipeline. Sharded engines are implemented and benchmarked, but
  `aetherd` still runs a single engine thread.
