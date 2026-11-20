#pragma once

#include "aether/core/types.hpp"

namespace aether {
struct PriceLevel;

// Orders carry their own links (intrusive list), so queuing allocates nothing.
// A PriceLevel owns a sentinel Link that closes the queue into a ring, so linking and
// unlinking never test for "first" or "last".
struct Link {
    Link* prev = nullptr;
    Link* next = nullptr;
};

struct Order : Link {
    OrderId      id    = 0;
    Price        price = 0;
    Qty          qty   = 0;
    SeqNo        seq   = 0;
    PriceLevel*  level = nullptr;
    InstrumentId instrument = 0;
    Side         side = Side::Buy;
};

// One order = one cache line = one miss.
static_assert(sizeof(Order) == 64);
}
