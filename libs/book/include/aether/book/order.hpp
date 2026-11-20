#pragma once

#include "aether/core/types.hpp"

namespace aether {

struct PriceLevel;

// The two pointers that thread an order into its price level's queue. A PriceLevel owns one
// Link of its own (a sentinel) that closes the queue into a ring, so an order always has a
// valid prev and next and linking or unlinking never has to test for "first" or "last".
struct Link {
    Link* prev = nullptr; // towards the front of the queue (older)
    Link* next = nullptr; // towards the back of the queue (newer)
};

// A resting order. It carries its own links (intrusive list), so queuing it at a price level
// allocates nothing. Sized to exactly one 64-byte cache line.
struct Order : Link {
    OrderId      id    = 0;
    Price        price = 0;
    Qty          qty   = 0;       // remaining
    SeqNo        seq   = 0;       // arrival sequence
    PriceLevel*  level = nullptr; // back-pointer for O(1) cancel
    InstrumentId instrument = 0;
    Side         side = Side::Buy;
};

static_assert(sizeof(Order) == 64);

} // namespace aether
