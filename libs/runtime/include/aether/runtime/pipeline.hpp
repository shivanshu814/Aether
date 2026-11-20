#pragma once

#include <atomic>
#include <cstdint>
#include <span>
#include <thread>

#include "aether/concurrency/spsc_ring.hpp"
#include "aether/engine/engine.hpp"
#include "aether/journal/event_log.hpp"
#include "aether/journal/hash.hpp"

// The threaded shape of the exchange:
//
//   gateway/sequencer --ring--> engine (owns all books) --ring--> publisher
//       (caller thread)            (1 thread)                      (1 thread)
//
// The engine thread is the only thread that touches the books, so no locks are needed and
// the output is exactly the same as running the engine on one thread.
namespace aether {

struct PipelineResult {
    std::uint64_t hash   = 0; // FNV-1a over all output events, in order
    std::uint64_t events = 0;
};

// Pushes events into an SPSC ring, spinning while the ring is full.
struct RingSink {
    SpscRing<Event>& ring;

    void emit(const Event& e) noexcept {
        while (!ring.try_push(e)) std::this_thread::yield();
    }
};
static_assert(EventSink<RingSink>);

// Runs `commands` through gateway -> engine -> publisher on separate threads and returns the
// hash of everything the publisher received. If `journal` is given, each command is written
// to the log before it is handed to the engine.
template <class Book>
PipelineResult run_pipeline(std::span<const BookConfig> instruments,
                            std::span<const Command> commands, LogWriter* journal = nullptr,
                            std::size_t ring_capacity = 1 << 16) {
    SpscRing<Command> inbound(ring_capacity);
    SpscRing<Event>   outbound(ring_capacity);
    std::atomic<bool> gateway_done{false};
    std::atomic<bool> engine_done{false};
    HashSink          published;

    std::thread engine_thread([&] {
        Engine<Book> engine(instruments);
        RingSink     sink{outbound};
        Command      c{};
        while (true) {
            if (inbound.try_pop(c)) {
                engine.process(c, sink);
            } else if (gateway_done.load(std::memory_order_acquire)) {
                // The flag was set after the last push, so one more pop sees everything.
                while (inbound.try_pop(c)) engine.process(c, sink);
                break;
            } else {
                std::this_thread::yield();
            }
        }
        engine_done.store(true, std::memory_order_release);
    });

    std::thread publisher_thread([&] {
        Event e{};
        while (true) {
            if (outbound.try_pop(e)) {
                published.emit(e);
            } else if (engine_done.load(std::memory_order_acquire)) {
                while (outbound.try_pop(e)) published.emit(e);
                break;
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Gateway + sequencer: journal first, then hand to the engine, strictly in order.
    for (const Command& c : commands) {
        if (journal != nullptr) journal->append(c);
        while (!inbound.try_push(c)) std::this_thread::yield();
    }
    gateway_done.store(true, std::memory_order_release);

    engine_thread.join();
    publisher_thread.join();
    return {published.hash(), published.count};
}

// The same work on the calling thread. This is the reference for run_pipeline.
template <class Book>
PipelineResult run_single_threaded(std::span<const BookConfig> instruments,
                                   std::span<const Command> commands) {
    Engine<Book> engine(instruments);
    HashSink     sink;
    for (const Command& c : commands) engine.process(c, sink);
    return {sink.hash(), sink.count};
}

} // namespace aether
