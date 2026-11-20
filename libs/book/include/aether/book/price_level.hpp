#pragma once

#include <cstdint>

#include "aether/book/order.hpp"

namespace aether {

// All orders resting at one price: an intrusive doubly linked FIFO, closed into a ring by a
// sentinel link. sentinel.next is the oldest order (fills first); sentinel.prev is the newest.
// Because of the sentinel, push_back and unlink are straight-line code with no branches.
struct PriceLevel {
    Link          sentinel;
    Qty           total = 0; // sum of remaining qty
    std::uint32_t count = 0;

    PriceLevel() noexcept { sentinel.prev = sentinel.next = &sentinel; }
    // The sentinel points at itself, so a level must never be copied or moved.
    PriceLevel(const PriceLevel&)            = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;

    bool empty() const noexcept { return sentinel.next == &sentinel; }

    // Oldest / newest order. Only valid when the level is not empty.
    Order* front() const noexcept { return static_cast<Order*>(sentinel.next); }
    Order* back() const noexcept { return static_cast<Order*>(sentinel.prev); }
    // True when a walk along next has come back round to the level itself.
    bool is_end(const Link* link) const noexcept { return link == &sentinel; }

    void push_back(Order* o) noexcept {
        Link* last    = sentinel.prev;
        o->level      = this;
        o->prev       = last;
        o->next       = &sentinel;
        last->next    = o;
        sentinel.prev = o;
        total += o->qty;
        ++count;
    }

    // O(1) removal from anywhere in the queue.
    void unlink(Order* o) noexcept {
        o->prev->next = o->next;
        o->next->prev = o->prev;
        total -= o->qty;
        --count;
    }
};

} // namespace aether
