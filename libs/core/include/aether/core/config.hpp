#pragma once

#include <cstddef>

#include "aether/core/types.hpp"

namespace aether {
struct BookConfig {
    Price       min_price   = 1;
    Price       max_price   = 20'000;
    std::size_t max_orders  = 1 << 16;
    OrderId     id_capacity = 1 << 20;

    friend bool operator==(const BookConfig&, const BookConfig&) = default;
};
}
