#pragma once

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

    std::size_t instrument_count() const noexcept { return books_.size(); }
    const Book& book(InstrumentId id) const { return books_[id]; }

private:
    std::vector<Book> books_;
};

} // namespace aether
