#pragma once

#include <cstddef>

#include "aether/core/types.hpp"

namespace aether {

// Static limits for one instrument's book. Everything the book will ever need is sized
// from this at construction, so the hot path never allocates.
struct BookConfig {
    Price       min_price   = 1;        // lowest valid limit price, in ticks
    Price       max_price   = 20'000;   // highest valid limit price, in ticks
    std::size_t max_orders  = 1 << 16;  // max orders resting at once
    OrderId     id_capacity = 1 << 20;  // valid order ids are [0, id_capacity)

    friend bool operator==(const BookConfig&, const BookConfig&) = default;
};

} // namespace aether
