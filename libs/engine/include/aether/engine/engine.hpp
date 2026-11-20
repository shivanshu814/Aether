#pragma once

#include <algorithm>
#include <span>
#include <vector>

#include "aether/core/config.hpp"
#include "aether/core/event_factory.hpp"
#include "aether/core/event_sink.hpp"

namespace aether {

// What the engine needs from a book type. RefOrderBook and OrderBook both satisfy it.
template <class B, class S>
concept BookFor = EventSink<S> && requires(B book, const Command& c, S& sink) {
    book.on_new(c, sink);
    book.on_cancel(c, sink);
    book.on_modify(c, sink);
};

// Routes each command to the book of its instrument. One book per instrument id.
// The engine is a pure function of the command stream: no clock, no I/O, no randomness.
template <class Book>
class Engine {
public:
    explicit Engine(std::span<const BookConfig> instruments) {
        books_.reserve(instruments.size());
        for (const BookConfig& cfg : instruments) books_.emplace_back(cfg);
    }

    template <EventSink S>
        requires BookFor<Book, S>
    void process(const Command& c, S& sink) {
        if (c.instrument >= books_.size()) {
            sink.emit(ev::reject(c, RejectReason::UnknownInstrument));
            return;
        }
        Book& book = books_[c.instrument];
        switch (c.type) {
        case CommandType::New:    book.on_new(c, sink); break;
        case CommandType::Cancel: book.on_cancel(c, sink); break;
        case CommandType::Modify: book.on_modify(c, sink); break;
        }
    }

    // Same result as calling process() on each command in order, but looks ahead: the
    // id-index slot of the command kFar ahead and the order of the command kNear ahead are
    // prefetched, so a cancel's two dependent cache misses are already in flight when the
    // command is reached. Hints are branch-free; the instrument is clamped, never branched on.
    template <EventSink S>
        requires BookFor<Book, S>
    void process_batch(std::span<const Command> commands, S& sink) {
        if constexpr (requires(const Book& b, OrderId id) { b.prefetch_index_slot(id); b.prefetch_order(id); }) {
#ifndef AETHER_PREFETCH_FAR
#define AETHER_PREFETCH_FAR 12
#define AETHER_PREFETCH_NEAR 4
#endif
            constexpr std::size_t kFar  = AETHER_PREFETCH_FAR;
            constexpr std::size_t kNear = AETHER_PREFETCH_NEAR;
            const std::size_t     n     = commands.size();
            const std::size_t     last  = books_.size() - 1;
            std::size_t           i     = 0;
            for (; i + kFar < n; ++i) {
                const Command& far  = commands[i + kFar];
                const Command& near = commands[i + kNear];
                books_[std::min<std::size_t>(far.instrument, last)].prefetch_index_slot(far.order_id);
                books_[std::min<std::size_t>(near.instrument, last)].prefetch_order(near.order_id);
                process(commands[i], sink);
            }
            for (; i < n; ++i) process(commands[i], sink);
        } else {
            for (const Command& c : commands) process(c, sink);
        }
    }

    std::size_t instrument_count() const noexcept { return books_.size(); }
    const Book& book(InstrumentId id) const { return books_[id]; }

private:
    std::vector<Book> books_;
};

} // namespace aether
