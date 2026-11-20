#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

#include "aether/concurrency/spsc_ring.hpp"
#include "aether/engine/engine.hpp"

// Sharding: N engine threads, each owning its own set of instruments.
//
//   instrument i  ->  shard (i % N),  where it becomes local instrument (i / N)
//
// Books for different instruments never share state, so shards need no locks and no
// communication. Every instrument still sees its own commands in their original order, so
// each shard's output is deterministic and identical to running that shard on one thread.
// What is NOT defined is an ordering between events of different shards.
namespace aether {

// Splits one command stream into per-shard streams, keeping the original order inside each
// shard and rewriting instrument ids to the shard-local id.
inline std::vector<std::vector<Command>> partition_by_shard(std::span<const Command> commands,
                                                            std::size_t shard_count) {
    std::vector<std::vector<Command>> shards(shard_count);
    for (std::vector<Command>& s : shards) s.reserve(commands.size() / shard_count + 1);
    for (const Command& c : commands) {
        Command local    = c;
        local.instrument = static_cast<InstrumentId>(c.instrument / shard_count);
        shards[c.instrument % shard_count].push_back(local);
    }
    return shards;
}

// The instruments each shard owns, in shard-local order.
inline std::vector<std::vector<BookConfig>> partition_instruments(std::span<const BookConfig> instruments,
                                                                  std::size_t shard_count) {
    std::vector<std::vector<BookConfig>> shards(shard_count);
    for (std::size_t i = 0; i < instruments.size(); ++i) shards[i % shard_count].push_back(instruments[i]);
    return shards;
}

// Pins the calling thread to one CPU. Only Linux can do this; elsewhere it does nothing and
// returns false (macOS has no call to bind a thread to a core).
inline bool pin_current_thread(std::size_t cpu) noexcept {
#if defined(__linux__)
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return pthread_setaffinity_np(pthread_self(), sizeof(set), &set) == 0;
#else
    (void)cpu;
    return false;
#endif
}

template <class Sink>
struct ShardedRun {
    std::vector<Sink>   sinks;         // one per shard, in shard order
    std::vector<double> shard_seconds; // how long each shard took for its own stream
    double              seconds = 0;   // wall time from the common start until the last shard finished
};

// Runs one engine per shard, each on its own thread. Engines are built before the clock
// starts; all threads are released together. With pin_threads, shard s is bound to CPU s
// (Linux only), so the scheduler cannot move it or stack two shards on one core.
template <class Book, class Sink>
ShardedRun<Sink> run_sharded(const std::vector<std::vector<BookConfig>>& shard_instruments,
                             const std::vector<std::vector<Command>>&    shard_commands,
                             bool                                        pin_threads = false) {
    using Clock = std::chrono::steady_clock;
    struct alignas(64) Slot { // one cache line per shard: no false sharing between sinks
        Sink              sink{};
        Clock::time_point finished{};
    };

    const std::size_t        shard_count = shard_commands.size();
    std::vector<Slot>        slots(shard_count);
    std::atomic<std::size_t> ready{0};
    std::atomic<bool>        go{false};
    std::vector<std::thread> threads;
    threads.reserve(shard_count);

    for (std::size_t s = 0; s < shard_count; ++s) {
        threads.emplace_back([&, s] {
            if (pin_threads) pin_current_thread(s);
            Engine<Book>                engine(shard_instruments[s]);
            const std::vector<Command>& commands = shard_commands[s];
            Sink&                       sink     = slots[s].sink;
            ready.fetch_add(1, std::memory_order_release);
            while (!go.load(std::memory_order_acquire)) cpu_relax();
            for (const Command& c : commands) engine.process(c, sink);
            slots[s].finished = Clock::now();
        });
    }
    while (ready.load(std::memory_order_acquire) != shard_count) std::this_thread::yield();
    const Clock::time_point start = Clock::now();
    go.store(true, std::memory_order_release);
    for (std::thread& t : threads) t.join();

    ShardedRun<Sink>  result;
    Clock::time_point last = start;
    for (Slot& slot : slots) {
        result.sinks.push_back(slot.sink);
        result.shard_seconds.push_back(std::chrono::duration<double>(slot.finished - start).count());
        if (slot.finished > last) last = slot.finished;
    }
    result.seconds = std::chrono::duration<double>(last - start).count();
    return result;
}

} // namespace aether
