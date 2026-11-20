# Aether: Ultra-low-latency Matching Engine

A miniature electronic exchange: price-time priority matching across multiple instruments,
with a custom order book, a memory pool, and deterministic event replay.

Headline numbers:

| Metric | Meaning |
|---|---|
| **Orders/sec** | Throughput: how many commands the engine finishes per second under sustained load |
| **p50 / p99 / p99.9** | Latency per command: typical case, 1-in-100 worst, 1-in-1000 worst |
| **Allocations per order** | Heap allocations on the hot path. Target in steady state: exactly **0** |

Measured on an Apple M1 Pro: **61.9 M commands/sec** on one engine thread with p99 83 ns,
p99.9 167 ns and **0** heap allocations per command; about 270 M commands/sec across 8
sharded engine threads. Method, full tables and the limits of these numbers are in
[BENCHMARKS.md](BENCHMARKS.md).

This document explains the design with diagrams and pseudocode. The implementation lives in
`libs/`; section 11 maps each part of the design to its file.

> Diagrams are **Mermaid**. They render on GitHub, and in VS Code with the
> "Markdown Preview Mermaid Support" extension (open preview with `Cmd+Shift+V`).

---

## Contents

1. [The big picture](#1-the-big-picture)
2. [Vocabulary](#2-vocabulary)
3. [Price-time priority](#3-price-time-priority)
4. [Order lifecycle](#4-order-lifecycle)
5. [Data structures](#5-data-structures)
6. [Memory pool](#6-memory-pool)
7. [Threads and the SPSC ring buffer](#7-threads-and-the-spsc-ring-buffer)
8. [Deterministic event replay](#8-deterministic-event-replay)
9. [Measuring the three headline numbers](#9-measuring-the-three-headline-numbers)
10. [Build plan](#10-build-plan)
11. [Repo layout](#11-repo-layout)

---

## 1. The big picture

Clients send orders. Something has to put every order into one global order, write that
order to disk, then hand it to the matcher. The matcher keeps one order book per instrument and
turns each command into output events (acks, trades, cancels).

```mermaid
flowchart LR
    C["Clients<br/>traders, bots"] -->|orders| G["Gateway<br/>decode + validate"]
    G -->|SPSC ring| S["Sequencer<br/>assigns seq no. + timestamp"]
    S -->|append| L[("Event log<br/>on disk")]
    S -->|commands, in order| E
    subgraph E["Matching engine: 1 thread, HOT PATH"]
        direction TB
        B1["book: AAPL"]
        B2["book: MSFT"]
        B3["book: BTCUSD"]
    end
    E -->|ack, fill, cancel| X["Exec reports"]
    E -->|book updates| M["Market data"]
    style E fill:#e2e9fb,stroke:#2c56c9,stroke-width:2px
```

**Key idea:** the engine never touches the network or the disk. It reads ordered commands from
one queue and writes events to another. That isolation makes it fast *and* makes replay possible.

---

## 2. Vocabulary

| Term | Meaning | In Aether |
|---|---|---|
| Instrument | The thing traded (AAPL, BTCUSD) | `uint32_t instrument_id`, one book each |
| Side | Buy (bid) or sell (ask) | `enum class Side : uint8_t` |
| Tick | Smallest price step, e.g. 0.01 | Prices are `int64_t` tick counts. **Never `double`** |
| Limit order | "Buy 100 at 101.02 or better." Leftover rests in the book | `OrderType::Limit` |
| Market order | "Buy 100 at any price." Never rests | `OrderType::Market` |
| IOC | Immediate-or-cancel: has a limit price, leftover is cancelled | `OrderType::IOC` |
| Best bid / ask | Highest buy price / lowest sell price | Cached, updated on every change |
| Spread | Best ask − best bid. Always > 0 in a resting book | If an order would make it ≤ 0, it matches |
| Maker / taker | Maker = resting order. Taker = incoming order that hits it | Trade price = **maker's** price |
| Fill / partial | Trade completes all / part of an order | One `Trade` event per maker touched |
| Cancel / modify | Remove an order / change it | Qty down keeps priority. Price change or qty up loses it |

---

## 3. Price-time priority

Two rules decide who trades first:

- **Price first:** the better price always trades first (highest bid, lowest ask).
- **Time second:** at the same price, whoever arrived first trades first.

So each price level is a **FIFO queue**, and the book is a **sorted collection of those queues**.

```mermaid
flowchart TB
    subgraph A3["ASK 101.03 | total 200"]
        direction LR
        o9["#9 · 200"]
    end
    subgraph A2["ASK 101.02 | total 400"]
        direction LR
        o4["#4 · 100"] --> o7["#7 · 300"]
    end
    subgraph A1["ASK 101.01 | total 300 | BEST ASK"]
        direction LR
        o2["#2 · 50"] --> o5["#5 · 150"] --> o8["#8 · 100"]
    end
    SP{{"spread = 101.01 − 100.99 = 0.02"}}
    subgraph B1["BID 100.99 | total 400 | BEST BID"]
        direction LR
        o1["#1 · 300"] --> o6["#6 · 100"]
    end
    subgraph B2["BID 100.98 | total 500"]
        direction LR
        o3["#3 · 500"]
    end
    subgraph B3["BID 100.97 | total 250"]
        direction LR
        o10["#10 · 250"]
    end
    A3 ~~~ A2 ~~~ A1 ~~~ SP ~~~ B1 ~~~ B2 ~~~ B3

    classDef ask fill:#fbe6da,stroke:#c24a12,color:#15212c
    classDef bid fill:#dcf1ec,stroke:#0b7a69,color:#15212c
    class o9,o4,o7,o2,o5,o8 ask
    class o1,o6,o3,o10 bid
```

How to read it:

- Each box is `#id · qty`. Ids were assigned in arrival order.
- Arrows inside a level go **front → back** of the queue. The leftmost order fills first.
- That's why #2 is ahead of #5 and #8 at 101.01.

### Walkthrough: incoming `Buy 450 @ 101.02`

A buyer looks at the **ask** side, starting from the best ask:

```mermaid
sequenceDiagram
    participant T as Taker: BUY 450 @ 101.02
    participant L1 as Ask level 101.01
    participant L2 as Ask level 101.02
    T->>L1: best ask 101.01 ≤ 101.02, crosses
    L1-->>T: #2 fills 50 @ 101.01 (taker left 400)
    L1-->>T: #5 fills 150 @ 101.01 (taker left 250)
    L1-->>T: #8 fills 100 @ 101.01 (taker left 150)
    Note over L1: level empty, removed. Best ask moves to 101.02
    T->>L2: best ask 101.02 ≤ 101.02, still crosses
    L2-->>T: #4 fills 100 @ 101.02 (taker left 50)
    L2-->>T: #7 fills 50 @ 101.02 (taker left 0)
    Note over L2: #7 keeps 250, stays at front
    Note over T: done: 5 trades, nothing rests
```

Book after:

| Side | Price | Queue |
|---|---|---|
| ASK | 101.03 | #9 · 200 |
| ASK | 101.02 | #7 · 250  ← new best ask |
| BID | 100.99 | #1 · 300, #6 · 100 |
| BID | 100.98 | #3 · 500 |
| BID | 100.97 | #10 · 250 |

- If it had been **Buy 800**, after sweeping 700 the leftover 100 would **rest** as a new bid
  at 101.02 and become the best bid.
- Trades print at the **resting (maker) order's price**, 101.01, not the taker's limit 101.02.
  The taker gets the better price.

### The matching loop as a flowchart

```mermaid
flowchart TD
    N["New order arrives"] --> Q{"qty > 0 AND<br/>opposite side not empty AND<br/>crosses best price?"}
    Q -- yes --> F["maker = front of best level<br/>fill = min(taker.qty, maker.qty)<br/>emit Trade at maker price"]
    F --> MZ{"maker.qty == 0?"}
    MZ -- yes --> RM["pop maker from level<br/>erase from index<br/>return slot to pool"]
    MZ -- no --> LE
    RM --> LE{"level empty?"}
    LE -- yes --> RL["remove level<br/>best price moves"] --> Q
    LE -- no --> Q
    Q -- no --> LEFT{"taker qty > 0?"}
    LEFT -- no --> DONE["done"]
    LEFT -- yes --> TY{"type?"}
    TY -- Limit --> REST["push to back of own level<br/>emit Rested"]
    TY -- "Market / IOC" --> EXP["emit Expired<br/>(never rests)"]
```

### Same thing as pseudocode

```text
on_new_order(o):
    opp = (o.side == Buy) ? asks : bids
    while o.qty > 0 and !opp.empty() and crosses(o, opp.best_price()):
        level   = opp.best_level()
        maker   = level.front()                    // time priority
        fill    = min(o.qty, maker.qty)
        emit Trade{ buyer, seller, price = level.price, qty = fill }
        o.qty  -= fill;  maker.qty -= fill;  level.total -= fill
        if maker.qty == 0: level.pop_front(); index.erase(maker.id); pool.free(maker)
        if level.empty():  opp.remove_level(level)  // best price moves
    if o.qty > 0:
        if o.type == Limit: own_side.level(o.price).push_back(o); emit Rested
        else:               emit Expired{ o.qty }   // Market / IOC never rest

crosses(o, p) = (o.type == Market) or (o.side == Buy ? p <= o.price : p >= o.price)

on_cancel(id):
    o = index.find(id)            // O(1)
    if !o: emit Reject; return
    o.level.unlink(o)             // O(1), intrusive list
    if o.level.empty(): remove_level(o.level)
    index.erase(id); pool.free(o); emit Canceled
```

Everything else in this project is about making this loop fast.

---

## 4. Order lifecycle

Every order is in exactly one of these states. The events (`Ack`, `Trade`, `Rested`,
`Expired`, `Canceled`, `Modified`, `Reject`) are the arrows.

```mermaid
stateDiagram-v2
    [*] --> Rejected: invalid (bad price, qty, pool full)
    [*] --> Matching: valid, Ack sent
    Matching --> Filled: qty reaches 0
    Matching --> Resting: Limit with qty left
    Matching --> Expired: Market or IOC with qty left
    Resting --> PartiallyFilled: a taker fills part
    PartiallyFilled --> PartiallyFilled: more partial fills
    Resting --> Filled: a taker fills all
    PartiallyFilled --> Filled: rest filled
    Resting --> Canceled: cancel request
    PartiallyFilled --> Canceled: cancel request
    Filled --> [*]
    Expired --> [*]
    Canceled --> [*]
    Rejected --> [*]
```

Modify rules:

- **Same price and quantity not above what is left:** changed in place, priority kept.
  One `Modified` event.
- **Price change or quantity up:** cancel + new limit order with the same id, at the back of
  the queue. Events: `Canceled`, `Ack`, then any `Trade`s, then `Rested` if anything is left.

What is rejected, and when (always before the `Ack`, so a rejected command changes nothing):

| Reason | Condition |
|---|---|
| `InvalidQty` | quantity is zero or negative |
| `InvalidPrice` | limit or IOC price outside the instrument's price band |
| `InvalidOrderId` | order id outside the instrument's id capacity |
| `DuplicateOrder` | a new order reuses the id of an order still resting |
| `BookFull` | a new limit order arrives while every order slot is in use |
| `UnknownOrder` | cancel or modify of an id that is not resting |
| `UnknownInstrument` | no book for that instrument id |

---

## 5. Data structures

The reference book (`RefOrderBook`) is `std::map<Price, std::deque<Order>>` per side plus a
`std::unordered_map` for id lookup. It is correct and was written first. It is slow because
new orders and new price levels call `new`, and every lookup chases pointers across memory.

The fast book needs three operations to be cheap:

| Operation | When | Target |
|---|---|---|
| Find best price | Every incoming order | O(1) |
| Push back / pop front at one price | Every rest / fill | O(1), no allocation |
| Cancel by id | Every cancel | id → pointer, then O(1) unlink |

### Book → PriceLevel → intrusive list of Orders

```mermaid
flowchart LR
    BK["OrderBook<br/>bids_[tick] → Level<br/>asks_[tick] → Level<br/>best_bid_, best_ask_"] --> PL["PriceLevel 101.01<br/>total 300 · count 3"]
    PL -->|head| O2
    PL -->|tail| O8
    subgraph Q["intrusive doubly linked FIFO"]
        direction LR
        O2["Order #2 · 50<br/>prev | next"] -->|next| O5["Order #5 · 150<br/>prev | next"] -->|next| O8["Order #8 · 100<br/>prev | next"]
        O8 -.->|prev| O5 -.->|prev| O2
    end
    subgraph IDX["Order index: id → Order*"]
        direction TB
        I2["id 2"]
        I5["id 5"]
        I8["id 8"]
    end
    I2 --> O2
    I5 --> O5
    I8 --> O8
    style PL fill:#e2e9fb,stroke:#2c56c9
```

- **Intrusive list:** each `Order` carries its own `prev`/`next` pointers, so no separate list
  node is ever allocated.
- **Cancel #5:** look up id 5 in the index → set `#2.next = #8` and `#8.prev = #2`. No search.

### Where price levels live: tree vs flat array

```mermaid
flowchart TB
    subgraph MAP["Option A: std::map#lt;Price, Level#gt; (red-black tree)"]
        direction TB
        r["100.97"] --> l["100.95"]
        r --> rr["100.99"]
        l --> ll["100.92"]
        nA["each node = 1 heap allocation<br/>best = walk to tree edge, O(log n)<br/>pointer chasing = cache misses"]
    end
    subgraph ARR["Option B: Level levels[N], index = price − min_price"]
        direction LR
        c0["0"] --- c1["1 ■"] --- c2["2"] --- c3["3 ■"] --- c4["4"] --- c5["5"] --- c6["6 ■"] --- c7["7"] --- c8["8"] --- c9["9"]
        BB(["best_bid_ = 6"]) --> c6
    end
    MAP ~~~ ARR
    classDef filled fill:#dcf1ec,stroke:#0b7a69,color:#15212c
    classDef empty fill:#ffffff,stroke:#999,stroke-dasharray:4 4,color:#566573
    class c1,c3,c6 filled
    class c0,c2,c4,c5,c7,c8,c9 empty
```

| | `std::map` | Flat array |
|---|---|---|
| Find best | O(log n), pointer walk | O(1), cached index |
| New level | heap allocation | none (allocated once) |
| Memory | only used levels | whole price band |
| Catch | slow | bounded price range; when best level empties, scan to next filled cell |

The flat array works because a real instrument trades in a known band (say ±20% around a
reference price, in ticks). A bitmap with one bit per level (`LevelBitmap`) makes "find the
next filled cell" a scan of 64 levels per machine word.

Both sides live in one array indexed by side (`sides_[Buy]`, `sides_[Sell]`), each holding
its levels, its bitmap and its cached best index. Resting and cancelling then pick the side
by index instead of branching on buy/sell, which measured about 5% faster.

Each price level closes its queue into a ring with a sentinel link of its own, so an order
always has a valid prev and next and linking or unlinking never tests for "first" or "last".

### Types

```cpp
// types.hpp
using Price   = std::int64_t;     // ticks, e.g. 101.02 with tick 0.01 -> 10102
using Qty     = std::int64_t;
using OrderId = std::uint64_t;
enum class Side      : std::uint8_t { Buy, Sell };
enum class OrderType : std::uint8_t { Limit, Market, IOC };

struct PriceLevel;
struct Link { Link* prev; Link* next; };   // intrusive FIFO links

struct Order : Link {             // exactly one 64-byte cache line (static_assert)
    OrderId       id;
    Price         price;
    Qty           qty;            // remaining
    std::uint64_t seq;            // arrival sequence = time priority
    PriceLevel*   level = nullptr;// back-pointer for O(1) cancel
    std::uint32_t instrument;
    Side          side;           // Market and IOC orders never rest, so no type is stored
};

struct PriceLevel {
    Link          sentinel;         // sentinel.next = oldest (fills first), sentinel.prev = newest
    Qty           total = 0;
    std::uint32_t count = 0;
    void push_back(Order*);         // no branches
    void unlink(Order*);            // no branches
    bool empty() const;             // sentinel.next == &sentinel
};
```

### Class diagram

```mermaid
classDiagram
    class Engine {
        -vector~OrderBook~ books_
        +process(Command, EventSink)
    }
    class OrderBook {
        -BookSide sides_[2]
        -vector~Order*~ index_
        -Pool~Order~ pool_
        +on_new(Command, EventSink)
        +on_cancel(Command, EventSink)
        +on_modify(Command, EventSink)
    }
    class BookSide {
        +vector~PriceLevel~ levels
        +LevelBitmap bits
        +size_t best
    }
    class PriceLevel {
        +Link sentinel
        +Qty total
        +push_back(Order*)
        +unlink(Order*)
    }
    class Order {
        +OrderId id
        +Price price
        +Qty qty
        +Order* prev
        +Order* next
        +PriceLevel* level
    }
    class Pool~T~ {
        -T** free_
        -size_t free_count_
        +create() T*
        +destroy(T*)
    }
    Engine "1" --> "many" OrderBook
    OrderBook "1" --> "2" BookSide
    BookSide "1" --> "many" PriceLevel
    PriceLevel "1" --> "many" Order : FIFO
    OrderBook --> Pool~T~ : allocates Orders
```

---

## 6. Memory pool

`new`/`delete` go to the general-purpose allocator. Usually fast, but occasionally it takes a
lock, asks the OS for pages, or walks a fragmented free list. Those occasional slow calls are
exactly what shows up at **p99.9**.

A pool allocates one big array of `Order` slots at startup. After that, getting and returning
an order is two pointer moves.

```mermaid
flowchart LR
    FH(["free_head"]) --> s2
    subgraph SLAB["one array of slots, allocated ONCE at startup"]
        direction LR
        s0["slot 0<br/>#2"] ~~~ s1["slot 1<br/>#5"] ~~~ s2["slot 2<br/>free"] ~~~ s3["slot 3<br/>#8"] ~~~ s4["slot 4<br/>free"] ~~~ s5["slot 5<br/>free"] ~~~ s6["slot 6<br/>#1"] ~~~ s7["slot 7<br/>free"]
    end
    s2 -.->|next| s4 -.->|next| s5 -.->|next| s7 -.->|next| NUL(["null"])
    classDef used fill:#dcf1ec,stroke:#0b7a69,color:#15212c
    classDef free fill:#ffffff,stroke:#999,stroke-dasharray:4 4,color:#566573
    class s0,s1,s3,s6 used
    class s2,s4,s5,s7 free
```

A free slot reuses its own bytes to store the "next free" pointer, so the list costs no extra
memory.

```mermaid
sequenceDiagram
    participant B as OrderBook
    participant P as Pool
    Note over P: free_head → slot 2 → slot 4 → ...
    B->>P: create()
    P-->>B: slot 2 (free_head = slot 4)
    Note over B: order #11 lives in slot 2
    B->>P: destroy(#5 in slot 1)
    Note over P: slot1.next = slot 4, free_head = slot 1
    B->>P: create()
    P-->>B: slot 1 again (still warm in cache)
```

```
allocate():                         deallocate(p):
    slot      = free_head               p->next   = free_head
    free_head = free_head->next         free_head = p
    return slot
```

Both O(1). No lock needed (only the engine thread touches it).

**What was built instead.** The diagrams above show the classic free list threaded through
the slots. It was implemented first and then replaced: with that list, every allocation has
to load the "next free" pointer out of the slot it is handing out before the following
allocation can start, so a burst of allocations queues up behind those loads. `Pool<T>` now
keeps the free slots on a separate stack of pointers (an array and a count). Same O(1)
operations and the same last-freed-first-reused order, but the next free slot is a
sequential array read. A burst of inserts went from 11.5 ns to 7.1 ns each; the cost is
8 bytes per slot.

```cpp
template <class T>
class Pool {
public:
    explicit Pool(std::size_t capacity);   // the only allocations; fill the free stack here
    template <class... Args>
    T*   create(Args&&... args);           // pop the stack, construct; nullptr when exhausted
    void destroy(T* p) noexcept;           // std::destroy_at(p), push the slot back
    std::size_t in_use() const noexcept;
private:
    struct Slot { alignas(T) std::byte storage[sizeof(T)]; };
    std::unique_ptr<Slot[]> slots_;
    std::unique_ptr<T*[]>   free_;        // stack of free slots
    std::size_t             free_count_;
};
```

> **When the pool is empty** the book rejects the order with `BookFull`, as a real exchange
> would, rather than calling `new`. The check happens before the `Ack`, and it is covered by
> a scenario test and by a differential test that runs with a 64-slot pool.

Other hidden allocations to remove from the hot path:

- `std::unordered_map` inserts → `reserve()` up front, or use a vector indexed by order id
- `std::vector::push_back` for output events → fixed pre-sized buffer
- `std::string` anywhere in the hot path
- `std::function` with captures

---

## 7. Threads and the SPSC ring buffer

Books for different instruments never share state. Simplest correct design:
**one engine thread owns all books** and processes commands one at a time. No locks inside the
engine. Other threads talk to it via **single-producer single-consumer (SPSC) ring buffers**,
which need only two atomic indexes.

```mermaid
flowchart LR
    P["Gateway thread<br/>(producer)"] -->|"push at tail"| r0
    subgraph R["Ring buffer, N = 8 (power of 2)"]
        direction LR
        r0["0"] --> r1["1"] --> r2["2 cmd ◀ head"] --> r3["3 cmd"] --> r4["4 cmd"] --> r5["5 cmd"] --> r6["6 ◀ tail"] --> r7["7"]
        r7 -->|"wrap: slot = i & (N-1)"| r0
    end
    r2 -->|"pop at head"| C["Engine thread<br/>(consumer, owns books)"]
    classDef full fill:#e2e9fb,stroke:#2c56c9,color:#15212c
    class r2,r3,r4,r5 full
```

The memory-ordering handshake (this is the part people get wrong):

```mermaid
sequenceDiagram
    participant Pr as Producer (gateway)
    participant S as slot[tail]
    participant T as tail (atomic)
    participant H as head (atomic)
    participant Co as Consumer (engine)
    Pr->>H: load head (acquire), check ring not full
    Pr->>S: write command
    Pr->>T: store tail+1 (RELEASE)
    Co->>T: load tail (ACQUIRE)
    Note over Co: release/acquire pair guarantees<br/>the slot write is visible now
    Co->>S: read command
    Co->>H: store head+1 (RELEASE)
```

Rules:

- Put `head` and `tail` on **separate 64-byte cache lines** (`alignas(64)`), otherwise the two
  cores fight over one line (false sharing).
- Capacity N is a power of two so wrap is a mask, not a modulo.

What the implementation does beyond the basic ring:

- **Batched hand-off.** `try_push_n` / `try_pop_n` move up to 256 items with one atomic
  update, so the two cores exchange a cache line per batch instead of per item. The engine
  thread flushes its output batch before it waits for more input, so events never sit
  buffered. This took the pipeline from 6.7 M to about 25 M commands/sec.
- **Spin, then yield.** A waiting thread spins briefly with a CPU pause hint and then yields
  the core (`Backoff`), which keeps progress even when there are fewer cores than threads.
- macOS cannot hard-pin threads to cores; Linux can (`pthread_setaffinity_np`).

### Sharding across cores

Books for different instruments never share state, so engines can be sharded: N engine
threads, instrument `i` goes to thread `i % N`. Each instrument still sees its own commands
in order, so every shard stays deterministic. There is no defined order between events of
different shards.

```mermaid
flowchart LR
    G["Gateway"] --> SQ["Sequencer"]
    SQ -->|"instrument % 2 == 0"| R0["ring 0"] --> E0["Engine thread 0<br/>AAPL, BTCUSD"]
    SQ -->|"instrument % 2 == 1"| R1["ring 1"] --> E1["Engine thread 1<br/>MSFT, ETHUSD"]
    E0 --> OUT["ring out"] --> PUB["Publisher"]
    E1 --> OUT2["ring out"] --> PUB
```

`run_sharded` implements this and `aether_bench --shards N` measures it: aggregate
throughput passes 200 M commands/sec at 6 engine threads and reaches about 270 M at 8.
Scaling is not linear: at 8 threads a shard runs at 34 to 51 M instead of 60 M.
Measurements rule out memory bandwidth, show cache is only part of it, and point to
scheduling, since macOS cannot pin threads to cores (see BENCHMARKS.md). The live `aetherd` pipeline still runs a single
engine thread.

---

## 8. Deterministic event replay

If the engine is a **pure function of its ordered input**, you can record the input once and
rerun it any time: debug a crash, recover after restart, or prove an optimisation changed
nothing.

```mermaid
flowchart LR
    subgraph LIVE["LIVE RUN"]
        direction LR
        C1["Sequenced cmds"] --> E1["Engine"] --> O1["Output events"] --> H1["hash 0x5e1f…"]
    end
    subgraph REPLAY["REPLAY"]
        direction LR
        LF[("Event log file")] --> E2["Fresh engine"] --> O2["Output events"] --> H2["hash 0x5e1f…"]
    end
    C1 -.->|written to disk| LF
    H1 <-->|"must be byte-identical"| H2
    style H1 fill:#dcf1ec,stroke:#0b7a69
    style H2 fill:#dcf1ec,stroke:#0b7a69
```

The log stores **inputs only**. Outputs are recomputed, so any nondeterminism inside the engine
is a bug.

### What breaks determinism

```mermaid
flowchart TD
    IN["Same input log"] --> ENG["Engine"]
    ENG --> SAME["Same output ✅"]
    K1["reading the clock<br/>inside matching"] -.->|breaks| ENG
    K2["rand() / random ids"] -.->|breaks| ENG
    K3["double prices<br/>(rounding differs)"] -.->|breaks| ENG
    K4["unordered_map iteration<br/>deciding output order"] -.->|breaks| ENG
    K5["two threads touching<br/>one book"] -.->|breaks| ENG
    classDef bad fill:#fbe6da,stroke:#c24a12,color:#15212c
    class K1,K2,K3,K4,K5 bad
```

Rules:

- The engine reads nothing but the command stream. No clock reads, no `rand()`, no environment.
- Timestamps come from the sequencer and are stored in the log.
- Order ids come from a counter or from the log.
- Integer prices only.
- One thread per book, so thread scheduling never decides order.

### Log record format

```cpp
struct LogRecord {               // 48 bytes, written raw
    std::uint64_t seq;
    std::uint64_t ts_ns;         // from sequencer
    std::uint8_t  type;          // New, Cancel, Modify
    std::uint8_t  side, order_type, pad;
    std::uint32_t instrument;
    std::int64_t  price, qty;
    std::uint64_t order_id;
};
static_assert(sizeof(LogRecord) == 48);
```

```mermaid
packet-beta
  0-63: "seq (u64)"
  64-127: "ts_ns (u64)"
  128-135: "type"
  136-143: "side"
  144-151: "order_type"
  152-159: "pad"
  160-191: "instrument (u32)"
  192-255: "price (i64)"
  256-319: "qty (i64)"
  320-383: "order_id (u64)"
```

The file starts with a 16-byte header (magic `AETHLOG1`, version, instrument count) and one
32-byte record per instrument holding its price band, order capacity and id capacity. A log
is therefore self-describing: `aether_replay` needs nothing but the file.

Output is hashed with FNV-1a applied one 64-bit field at a time (`HashSink`), never over raw
struct bytes, so padding and byte order cannot affect it. The tests check that a journaled
run replays to the same hash, that the reference and fast books hash the same, that the
threaded pipeline matches the single-threaded engine, and they pin a golden hash for a
fixed seeded stream.

---

## 9. Measuring the three headline numbers

### Latency percentiles: why p99.9 matters more than the average

Record how long each command takes (engine dequeue → last output event written). Sort all
durations: **p50** is the middle one, **p99** is beaten by 99% of commands, **p99.9** by 99.9%.

Example distribution of 200,000 commands (illustrative numbers):

```mermaid
xychart-beta
    title "How many commands took how long"
    x-axis ["<200ns", "200-300", "300-400", "400-500", "500-700", "0.7-1µs", "1-2µs", "2-5µs", ">5µs"]
    y-axis "commands" 0 --> 90000
    bar [800, 52000, 88000, 38000, 15000, 4000, 1600, 400, 200]
```

The bars on the right are almost invisible, but that's where the pain is. Same data as
percentiles:

```mermaid
xychart-beta
    title "Latency by percentile (ns): the tail explodes"
    x-axis ["p50", "p90", "p99", "p99.9", "p99.99", "max"]
    y-axis "nanoseconds" 0 --> 16000
    line [350, 520, 1100, 4800, 9000, 15000]
```

Most commands are fast. Cache misses, page faults and OS interrupts create the long tail, and
that tail is what a trader feels on a busy day. An average hides it completely.

### Where the clock starts and stops

```mermaid
sequenceDiagram
    participant LG as Load generator
    participant RB as Ring in
    participant EN as Engine
    participant RO as Ring out
    Note over LG: t_sched = when order SHOULD be sent
    LG->>RB: push command
    RB->>EN: pop
    Note over EN: t0 = steady_clock::now()
    EN->>EN: match
    EN->>RO: push last event
    Note over EN: t1 = now(). Engine latency = t1 − t0
    RO->>LG: event received
    Note over LG: end-to-end = now − t_sched (avoids coordinated omission)
```

### How to measure each metric

| Metric | How | Traps |
|---|---|---|
| **Orders/sec** | Pre-generate N = 10M commands from a **seeded** RNG (~60% new limit, 30% cancel, 10% aggressive). Time the engine processing all of them. N / seconds. Run 3+ times, report median. | Generating orders inside the timed loop measures the generator. Do a warm-up run first. |
| **p50/p99/p99.9** | `std::chrono::steady_clock` before/after each command. Record into a **pre-allocated log-scale histogram** (HdrHistogram style), never a growing vector. | **Coordinated omission:** if the load generator waits for each reply before sending the next, slow periods send fewer orders and hide themselves. Send on a fixed schedule; measure from the *scheduled* send time for end-to-end numbers. |
| **Allocations/order** | Replace global `operator new` with a counting version in the bench binary. allocs in timed window ÷ commands. | Count only after warm-up (pools allocate at startup on purpose). Target: `0.000`. |

```cpp
// bench/alloc_counter.cpp — link only into the benchmark
inline std::atomic<std::uint64_t> g_allocs{0};

void* operator new(std::size_t n) {
    g_allocs.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(n)) return p;
    throw std::bad_alloc{};
}
void operator delete(void* p) noexcept              { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
```

> **Mac notes:** `rdtsc` is x86-only; on Apple Silicon use `steady_clock` (reads the ARM system
> counter, ~40 ns resolution). Profile with Instruments (Time Profiler, Allocations). Always
> report machine, compiler and flags next to every number.

---

## 10. Build plan

Order matters: **correctness → a way to measure → speed.** The slow version written first
becomes the oracle every fast version is checked against.

**Status: all eight milestones are implemented**, plus engine sharding. The list below is
kept as the record of the order the work was done in and what "done" meant for each step.

```mermaid
flowchart LR
    M1["1 · skeleton + types"] --> M2["2 · reference engine<br/>(std::map, slow)"] --> M3["3 · event log + replay"] --> M4["4 · benchmark baseline"]
    M4 --> M5["5 · pool + intrusive FIFO"] --> M6["6 · flat price array + id index"] --> M7["7 · multi-instrument + threads"] --> M8["8 · tune"]
    M2 -.->|"oracle: differential test"| M5
    M2 -.->|"oracle"| M6
    M3 -.->|"golden hash"| M7
    M4 -.->|"before / after numbers"| M8
    style M2 fill:#dcf1ec,stroke:#0b7a69
```

1. **Project skeleton and types.** CMake, C++20, GoogleTest. `types.hpp`, command/event structs.
   Debug build with `-fsanitize=address,undefined`, Release with `-O3`.
   *Done when:* an empty test runs in both builds.

2. **Reference engine (slow, obviously correct).** One book: `std::map<Price, std::deque<Order>>`
   per side + `std::unordered_map` for id lookup. Limit, Market, IOC, cancel, modify.
   *Done when:* hand-written tests pass, including the walkthrough (Buy 450 @ 101.02 → exactly 5 trades).

3. **Event log and replay.** Binary `LogRecord` writer/reader. `replay` tool prints FNV-1a hash
   of all output events.
   *Done when:* record + replay of 1M random commands gives the same hash; golden-hash test in CI.

4. **Benchmark harness.** Seeded load generator, timed loop, log-scale histogram, alloc counter.
   Prints orders/sec, p50/p99/p99.9, allocs/order.
   *Done when:* baseline numbers for the reference engine are written down.

5. **Memory pool + intrusive FIFO.** Write `Pool<T>` and intrusive `PriceLevel`. New `OrderBook`
   uses them; keep the reference one.
   *Done when:* differential test (both engines, same random stream) matches and allocs/order = 0.

6. **Flat price array + id index.** Tick-indexed array with cached best indexes; vector indexed
   by order id instead of hash map.
   *Done when:* differential test passes and p99.9 has dropped vs milestone 4.

7. **Multiple instruments + threads.** Book per instrument, SPSC rings in/out, gateway and
   publisher threads.
   *Done when:* replaying a multi-instrument log through the threaded build gives the same hash
   as single-threaded.

8. **Tune.** Cache-line align hot structs, check `sizeof(Order)`, reduce branches in the match
   loop, bitmap for next non-empty level. Profile before each change.
   *Done when:* every improvement has a before/after number.

### Differential test (milestones 5 and 6)

```mermaid
flowchart LR
    SEED["seeded random<br/>command stream"] --> REF["Reference engine<br/>(std::map)"] --> HR["events"]
    SEED --> FAST["Fast engine<br/>(pool + array)"] --> HF["events"]
    HR --> CMP{"identical?"}
    HF --> CMP
    CMP -- yes --> OK["pass ✅"]
    CMP -- no --> BUG["print first differing event<br/>+ seed → reproducible bug"]
```

---

## 11. Repo layout

```
aether/
├── CMakeLists.txt, CMakePresets.json     debug (ASan+UBSan), release, tsan
├── libs/                                 one small library per folder
│   ├── core/         types.hpp           Price, Qty, OrderId, Side, OrderType
│   │                 events.hpp          Command in; Event out
│   │                 event_sink.hpp      EventSink concept
│   │                 event_factory.hpp   builds events from the command that caused them
│   │                 config.hpp          BookConfig: price band and capacities
│   ├── memory/       pool.hpp            fixed-capacity object pool
│   ├── concurrency/  spsc_ring.hpp       lock-free SPSC ring, batching, Backoff
│   ├── reference/    ref_order_book.hpp  slow std::map book, kept as the test oracle
│   ├── book/         order.hpp           Order with intrusive prev/next
│   │                 price_level.hpp     FIFO of orders at one price
│   │                 level_bitmap.hpp    one bit per non-empty level
│   │                 order_book.hpp      the fast book
│   ├── engine/       engine.hpp          Engine<Book>: instrument id -> book
│   ├── journal/      event_log.hpp       LogRecord writer / reader
│   │                 hash.hpp            FNV-1a HashSink
│   ├── sim/          load_gen.hpp        deterministic seeded order flow
│   └── runtime/      pipeline.hpp        gateway -> engine -> publisher threads
│                     sharded.hpp         one engine thread per group of instruments
├── apps/
│   ├── aetherd/                          the threaded exchange on generated flow
│   └── replay/                           replay a log file, print the output hash
├── bench/                                benchmark, latency histogram, alloc counter
├── tests/                                unit, scenario, differential, replay
└── docs/                                 this document, BENCHMARKS.md
```

Which library depends on which (each can only include what it links):

```mermaid
flowchart BT
    core["core"]
    memory["memory"]
    concurrency["concurrency"]
    reference["reference"] --> core
    book["book"] --> core
    book --> memory
    engine["engine"] --> core
    journal["journal"] --> core
    sim["sim"] --> core
    runtime["runtime"] --> engine
    runtime --> journal
    runtime --> concurrency
    apps["apps / bench / tests"] --> runtime
    apps --> book
    apps --> reference
    apps --> sim
```

`engine` does not depend on `book` or `reference`: `Engine<Book>` is a template, and the book
type is chosen where it is used. That is what lets the same engine run the oracle and the
fast book in the differential tests.
