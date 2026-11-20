#pragma once

#include <cstdint>

#include "aether/book/order.hpp"

namespace aether {
// FIFO of orders at one price. sentinel.next is the oldest (fills first), sentinel.prev the newest.
struct PriceLevel {
    Link          sentinel;
    Qty           total = 0;
    std::uint32_t count = 0;

    PriceLevel() noexcept { sentinel.prev = sentinel.next = &sentinel; }

    // The sentinel points at itself, so a level must never be copied or moved.
    PriceLevel(const PriceLevel&)            = delete;
    PriceLevel& operator=(const PriceLevel&) = delete;

    bool empty() const noexcept { return sentinel.next == &sentinel; }

    Order* front() const noexcept { return static_cast<Order*>(sentinel.next); }
    Order* back() const noexcept { return static_cast<Order*>(sentinel.prev); }

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

    void unlink(Order* o) noexcept {
        o->prev->next = o->next;
        o->next->prev = o->prev;
        total -= o->qty;
        --count;
    }
};
}
