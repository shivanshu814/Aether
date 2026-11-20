#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <thread>

#include "aether/concurrency/spsc_ring.hpp"
#include "aether/engine/engine.hpp"
#include "aether/journal/event_log.hpp"
#include "aether/journal/hash.hpp"

namespace aether {
struct PipelineResult {
    std::uint64_t hash   = 0;
    std::uint64_t events = 0;
};

// gateway/sequencer (caller) --ring--> engine thread (owns all books) --ring--> publisher.
// Only the engine thread touches the books, so no locks and exactly the single-thread output.
inline constexpr std::size_t kBatch = 256;

class BatchingRingSink {
public:
    explicit BatchingRingSink(SpscRing<Event>& ring) : ring_(ring) {}

    void emit(const Event& e) noexcept {
        buffer_[size_++] = e;
        if (size_ == buffer_.size()) flush();
    }

    // Called after every drained batch so events never sit buffered while waiting for input.
    void flush() noexcept {
        std::size_t sent = 0;
        Backoff     backoff;
        while (sent < size_) {
            const std::size_t n = ring_.try_push_n(buffer_.data() + sent, size_ - sent);
            if (n == 0) {
                backoff.pause();
            } else {
                sent += n;
                backoff.reset();
            }
        }
        size_ = 0;
    }

private:
    SpscRing<Event>&          ring_;
    std::array<Event, kBatch> buffer_{};
    std::size_t               size_ = 0;
};
static_assert(EventSink<BatchingRingSink>);

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
        Engine<Book>                engine(instruments);
        BatchingRingSink            sink(outbound);
        std::array<Command, kBatch> batch{};
        Backoff                     backoff;
        bool                        draining = false;
        while (true) {
            const std::size_t n = inbound.try_pop_n(batch.data(), batch.size());
            if (n != 0) {
                engine.process_batch(std::span<const Command>(batch.data(), n), sink);
                sink.flush();
                backoff.reset();
            } else if (draining) {
                break;
            } else if (gateway_done.load(std::memory_order_acquire)) {
                draining = true;
            } else {
                backoff.pause();
            }
        }
        engine_done.store(true, std::memory_order_release);
    });

    std::thread publisher_thread([&] {
        std::array<Event, kBatch> batch{};
        Backoff                   backoff;
        bool                      draining = false;
        while (true) {
            const std::size_t n = outbound.try_pop_n(batch.data(), batch.size());
            if (n != 0) {
                for (std::size_t i = 0; i < n; ++i) published.emit(batch[i]);
                backoff.reset();
            } else if (draining) {
                break;
            } else if (engine_done.load(std::memory_order_acquire)) {
                draining = true;
            } else {
                backoff.pause();
            }
        }
    });

    std::size_t sent = 0;
    Backoff     backoff;
    while (sent < commands.size()) {
        const std::size_t want = std::min(kBatch, commands.size() - sent);
        if (journal != nullptr) {
            for (std::size_t i = 0; i < want; ++i) journal->append(commands[sent + i]);
            std::size_t pushed = 0;
            while (pushed < want) {
                const std::size_t n = inbound.try_push_n(commands.data() + sent + pushed, want - pushed);
                if (n == 0) {
                    backoff.pause();
                } else {
                    pushed += n;
                    backoff.reset();
                }
            }
            sent += want;
        } else {
            const std::size_t n = inbound.try_push_n(commands.data() + sent, want);
            if (n == 0) {
                backoff.pause();
            } else {
                sent += n;
                backoff.reset();
            }
        }
    }
    gateway_done.store(true, std::memory_order_release);

    engine_thread.join();
    publisher_thread.join();
    return {published.hash(), published.count};
}

template <class Book>
PipelineResult run_single_threaded(std::span<const BookConfig> instruments,
                                   std::span<const Command> commands) {
    Engine<Book> engine(instruments);
    HashSink     sink;
    engine.process_batch(commands, sink);
    return {sink.hash(), sink.count};
}
}
