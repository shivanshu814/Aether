#pragma once

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#include "aether/book/level_bitmap.hpp"
#include "aether/book/order.hpp"
#include "aether/book/price_level.hpp"
#include "aether/core/config.hpp"
#include "aether/core/event_factory.hpp"
#include "aether/core/event_sink.hpp"
#include "aether/memory/pool.hpp"

namespace aether {
// The fast book. Same events as RefOrderBook, but after construction nothing allocates:
//   price levels -> flat arrays indexed by (price - min_price), one per side
//   best price   -> cached index, repaired with the bitmap when a level empties
//   side         -> per-side state in an array indexed by side, so resting and cancelling
//                   select the side instead of branching on it
//   id lookup    -> vector indexed by order id
//   orders       -> fixed pool, one 64-byte line each
class OrderBook {
public:
    explicit OrderBook(const BookConfig& cfg)
        : cfg_(cfg),
          levels_(static_cast<std::size_t>(cfg.max_price - cfg.min_price + 1)),
          sides_{BookSide(levels_), BookSide(levels_)},
          index_(static_cast<std::size_t>(cfg.id_capacity), nullptr),
          pool_(cfg.max_orders) {}

    template <EventSink S>
    void on_new(const Command& c, S& sink) {
        if (c.qty <= 0) [[unlikely]] {
            sink.emit(ev::reject(c, RejectReason::InvalidQty));
            return;
        }
        if (c.order_type != OrderType::Market && !price_ok(c.price)) [[unlikely]] {
            sink.emit(ev::reject(c, RejectReason::InvalidPrice));
            return;
        }
        if (c.order_id >= cfg_.id_capacity) [[unlikely]] {
            sink.emit(ev::reject(c, RejectReason::InvalidOrderId));
            return;
        }
        if (index_[c.order_id] != nullptr) [[unlikely]] {
            sink.emit(ev::reject(c, RejectReason::DuplicateOrder));
            return;
        }
        if (c.order_type == OrderType::Limit && pool_.full()) [[unlikely]] {
            sink.emit(ev::reject(c, RejectReason::BookFull));
            return;
        }
        execute(c, sink);
    }

    template <EventSink S>
    void on_cancel(const Command& c, S& sink) {
        Order* o = find(c.order_id);
        if (o == nullptr) {
            sink.emit(ev::reject(c, RejectReason::UnknownOrder));
            return;
        }
        sink.emit(ev::canceled(c, o->side, o->price, o->qty));
        remove(o);
    }

    template <EventSink S>
    void on_modify(const Command& c, S& sink) {
        if (c.qty <= 0) [[unlikely]] {
            sink.emit(ev::reject(c, RejectReason::InvalidQty));
            return;
        }
        Order* o = find(c.order_id);
        if (o == nullptr) {
            sink.emit(ev::reject(c, RejectReason::UnknownOrder));
            return;
        }
        if (!price_ok(c.price)) [[unlikely]] {
            sink.emit(ev::reject(c, RejectReason::InvalidPrice));
            return;
        }
        if (c.price == o->price && c.qty <= o->qty) {
            o->level->total -= o->qty - c.qty;
            o->qty = c.qty;
            sink.emit(ev::modified(c, o->side, o->price, c.qty));
            return;
        }
        const Side side = o->side;
        sink.emit(ev::canceled(c, side, o->price, o->qty));
        remove(o);

        Command fresh    = c;
        fresh.type       = CommandType::New;
        fresh.side       = side;
        fresh.order_type = OrderType::Limit;
        execute(fresh, sink);
    }

    // Lookahead hints used by Engine::process_batch. Deliberately branch-free: a first
    // version that branched on the lookahead command's type was much slower than no
    // prefetch at all. The id is clamped, and prefetching nullptr is a no-op.
    void prefetch_index_slot(OrderId id) const noexcept {
        __builtin_prefetch(&index_[id < cfg_.id_capacity ? id : 0], 1);
    }
    void prefetch_order(OrderId id) const noexcept {
        __builtin_prefetch(index_[id < cfg_.id_capacity ? id : 0], 1);
    }

    std::optional<Price> best_bid() const {
        const std::size_t best = sides_[kBid].best;
        return best == kNone ? std::nullopt : std::optional<Price>(price_of(best));
    }
    std::optional<Price> best_ask() const {
        const std::size_t best = sides_[kAsk].best;
        return best == kNone ? std::nullopt : std::optional<Price>(price_of(best));
    }
    std::size_t order_count() const noexcept { return pool_.in_use(); }
    Qty         depth_at(Side side, Price price) const {
        if (!price_ok(price)) return 0;
        return sides_[static_cast<std::size_t>(side)].levels[index_of(price)].total;
    }

private:
    static constexpr std::size_t kNone = LevelBitmap::npos;
    static constexpr std::size_t kBid  = static_cast<std::size_t>(Side::Buy);
    static constexpr std::size_t kAsk  = static_cast<std::size_t>(Side::Sell);

    struct BookSide {
        explicit BookSide(std::size_t level_count) : levels(level_count), bits(level_count) {}
        std::vector<PriceLevel> levels;
        LevelBitmap             bits;
        std::size_t             best = kNone;
    };

    bool        price_ok(Price p) const noexcept { return p >= cfg_.min_price && p <= cfg_.max_price; }
    std::size_t index_of(Price p) const noexcept { return static_cast<std::size_t>(p - cfg_.min_price); }
    Price       price_of(std::size_t i) const noexcept { return cfg_.min_price + static_cast<Price>(i); }

    Order* find(OrderId id) const noexcept {
        return id < cfg_.id_capacity ? index_[id] : nullptr;
    }

    // Can the incoming order trade against the best level opposite? Written with selects,
    // not branches on side; for a passive order it is almost always false, which predicts well.
    bool crosses_best(const Command& c) const noexcept {
        const bool        buy  = c.side == Side::Buy;
        const std::size_t best = sides_[buy ? kAsk : kBid].best;
        if (best == kNone) return false;
        const Price price    = price_of(best);
        const bool  in_limit = buy ? price <= c.price : price >= c.price;
        return in_limit || c.order_type == OrderType::Market;
    }

    template <EventSink S>
    void execute(const Command& c, S& sink) {
        sink.emit(ev::ack(c));
        Qty remaining = c.qty;
        if (crosses_best(c)) {
            if (c.side == Side::Buy) {
                match_buy(c, remaining, sink);
            } else {
                match_sell(c, remaining, sink);
            }
            if (remaining == 0) return;
        }
        if (c.order_type == OrderType::Limit) [[likely]] {
            rest(c, remaining);
            sink.emit(ev::rested(c, remaining));
        } else {
            sink.emit(ev::expired(c, remaining));
        }
    }

    template <EventSink S>
    void sweep_level(PriceLevel& level, Price price, const Command& taker, Qty& remaining, S& sink) {
        Link* link = level.sentinel.next;
        while (remaining > 0 && !level.is_end(link)) {
            Order* maker = static_cast<Order*>(link);
            const Qty fill = std::min(remaining, maker->qty);
            sink.emit(ev::trade(taker, maker->id, price, fill));
            remaining -= fill;
            maker->qty -= fill;
            level.total -= fill;
            if (maker->qty != 0) break;
            link = maker->next;
            level.unlink(maker);
            index_[maker->id] = nullptr;
            pool_.destroy(maker);
        }
    }

    template <EventSink S>
    void match_buy(const Command& c, Qty& remaining, S& sink) {
        const bool market = c.order_type == OrderType::Market;
        BookSide&  asks   = sides_[kAsk];
        while (remaining > 0 && asks.best != kNone) {
            const Price price = price_of(asks.best);
            if (!market && price > c.price) break;
            PriceLevel& level = asks.levels[asks.best];
            sweep_level(level, price, c, remaining, sink);
            if (level.empty()) {
                asks.bits.clear(asks.best);
                asks.best = asks.bits.next_at_or_above(asks.best + 1);
            }
        }
    }

    template <EventSink S>
    void match_sell(const Command& c, Qty& remaining, S& sink) {
        const bool market = c.order_type == OrderType::Market;
        BookSide&  bids   = sides_[kBid];
        while (remaining > 0 && bids.best != kNone) {
            const Price price = price_of(bids.best);
            if (!market && price < c.price) break;
            PriceLevel& level = bids.levels[bids.best];
            sweep_level(level, price, c, remaining, sink);
            if (level.empty()) {
                bids.bits.clear(bids.best);
                bids.best = bids.best == 0 ? kNone : bids.bits.prev_at_or_below(bids.best - 1);
            }
        }
    }

    void rest(const Command& c, Qty remaining) noexcept {
        Order* o      = pool_.create();
        o->id         = c.order_id;
        o->price      = c.price;
        o->qty        = remaining;
        o->seq        = c.seq;
        o->instrument = c.instrument;
        o->side       = c.side;

        const std::size_t i    = index_of(c.price);
        const bool        buy  = c.side == Side::Buy;
        BookSide&         side = sides_[static_cast<std::size_t>(c.side)];
        side.levels[i].push_back(o);
        side.bits.set(i);

        const bool better = side.best == kNone || (buy ? i > side.best : i < side.best);
        side.best         = better ? i : side.best;
        index_[c.order_id] = o;
    }

    void remove(Order* o) noexcept {
        PriceLevel*       level = o->level;
        const std::size_t i     = index_of(o->price);
        BookSide&         side  = sides_[static_cast<std::size_t>(o->side)];
        level->unlink(o);
        const bool emptied = level->empty();
        side.bits.clear_if(i, emptied);
        if (emptied && i == side.best) [[unlikely]] {
            if (o->side == Side::Buy) {
                side.best = i == 0 ? kNone : side.bits.prev_at_or_below(i - 1);
            } else {
                side.best = side.bits.next_at_or_above(i + 1);
            }
        }
        index_[o->id] = nullptr;
        pool_.destroy(o);
    }

    BookConfig              cfg_;
    std::size_t             levels_;
    std::array<BookSide, 2> sides_;
    std::vector<Order*>     index_;
    Pool<Order>             pool_;
};
}
