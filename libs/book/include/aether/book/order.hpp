#pragma once

#include "aether/core/types.hpp"

namespace aether {

struct PriceLevel;

// A resting order. It carries its own prev/next links (intrusive list), so queuing it at a
// price level allocates nothing. Sized to exactly one 64-byte cache line.
struct Order {
    OrderId      id    = 0;
    Price        price = 0;
    Qty          qty   = 0;       // remaining
    SeqNo        seq   = 0;       // arrival sequence
    Order*       prev  = nullptr; // towards the front of the queue (older)
    Order*       next  = nullptr; // towards the back of the queue (newer)
    PriceLevel*  level = nullptr; // back-pointer for O(1) cancel
    InstrumentId instrument = 0;
    Side         side = Side::Buy;
};

static_assert(sizeof(Order) == 64);

} // namespace aether
