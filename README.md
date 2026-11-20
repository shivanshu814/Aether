# Aether

Ultra-low-latency matching engine in C++20: price-time priority order book, zero-allocation
hot path, lock-free SPSC rings and deterministic event replay.

Aether is a miniature electronic exchange. Orders come in and are given a global sequence
number. A single-threaded engine matches them by price-time priority, with one order book per
instrument, and emits acks, trades and cancels. The engine is a pure function of its input
stream, so any run can be replayed from the log and checked hash-for-hash.

## Status

- [x] **M1** · Project skeleton, core types and events
- [ ] **M2** · Reference engine (`std::map`, kept as the test oracle)
- [ ] **M3** · Event log and deterministic replay
- [ ] **M4** · Benchmark harness and baseline numbers
- [ ] **M5** · Memory pool and intrusive FIFO price levels
- [ ] **M6** · Flat price array and order-id index
- [ ] **M7** · Multiple instruments and threads (SPSC rings)
- [ ] **M8** · Tuning

Target metrics: orders/sec, p50 / p99 / p99.9 latency per command, and **0** heap
allocations per order in steady state. Results will be published here once measured.

## Requirements

- CMake 3.25+
- Ninja
- A C++20 compiler (developed with Apple clang 21 on Apple Silicon)

GoogleTest is downloaded automatically by CMake.

```bash
brew install cmake ninja   # macOS
```

## Build and test

```bash
# Debug: -O0 -g with AddressSanitizer + UndefinedBehaviorSanitizer
cmake --preset debug
cmake --build --preset debug
ctest --preset debug

# Release: -O3, used for all reported numbers
cmake --preset release
cmake --build --preset release
ctest --preset release
```

## Project layout

```
aether/
├── libs/core/      types (Price, Qty, Side, ...) and Command / Event structs
├── tests/unit/     GoogleTest unit tests
└── docs/           design document
```

New libraries (`reference`, `engine`, `journal`, `book`, ...) are added under `libs/` as each
milestone lands.

## Design

The full design, with diagrams, pseudocode and the build plan, is in
[docs/AETHER_DESIGN.md](docs/AETHER_DESIGN.md).

Key rules:

- Prices are integer ticks, never floating point.
- The engine never reads the clock, the network or the disk. It only sees sequenced commands.
- All memory is allocated at startup. The hot path never calls `new`.
- `Command` and `Event` are fixed 48-byte, trivially copyable structs, enforced by
  `static_assert`.
