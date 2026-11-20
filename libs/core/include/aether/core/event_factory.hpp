#pragma once

#include "aether/core/events.hpp"

// Small helpers that build output events from the command that caused them.
// Both the reference book and the fast book use these, so their events have the same shape.
namespace aether::ev {

constexpr Event base(const Command& c, EventType type) noexcept {
    Event e{};
    e.seq        = c.seq;
    e.order_id   = c.order_id;
    e.instrument = c.instrument;
    e.type       = type;
    e.side       = c.side;
    return e;
}

constexpr Event ack(const Command& c) noexcept {
    Event e = base(c, EventType::Ack);
    e.price = c.price;
    e.qty   = c.qty;
    return e;
}

constexpr Event reject(const Command& c, RejectReason reason) noexcept {
    Event e  = base(c, EventType::Reject);
    e.reason = reason;
    return e;
}

// price is the maker's price; qty is the fill size; side is the taker's side.
constexpr Event trade(const Command& taker, OrderId maker_id, Price price, Qty qty) noexcept {
    Event e    = base(taker, EventType::Trade);
    e.maker_id = maker_id;
    e.price    = price;
    e.qty      = qty;
    return e;
}

constexpr Event rested(const Command& c, Qty remaining) noexcept {
    Event e = base(c, EventType::Rested);
    e.price = c.price;
    e.qty   = remaining;
    return e;
}

constexpr Event expired(const Command& c, Qty remaining) noexcept {
    Event e = base(c, EventType::Expired);
    e.qty   = remaining;
    return e;
}

constexpr Event canceled(const Command& c, Side side, Price price, Qty remaining) noexcept {
    Event e = base(c, EventType::Canceled);
    e.side  = side;
    e.price = price;
    e.qty   = remaining;
    return e;
}

constexpr Event modified(const Command& c, Side side, Price price, Qty new_qty) noexcept {
    Event e = base(c, EventType::Modified);
    e.side  = side;
    e.price = price;
    e.qty   = new_qty;
    return e;
}

} // namespace aether::ev
