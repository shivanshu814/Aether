#pragma once

#include <algorithm>
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

// The fast order book. Same behaviour as RefOrderBook, event for event, but:
//   price levels -> flat arrays indexed by (price - min_price), one per side
//   best price   -> cached index, repaired with a bitmap when a level empties
//   FIFO         -> intrusive list threaded through the orders themselves
//   id lookup    -> vector indexed by order id
//   orders       -> fixed pool allocated once
// After construction no operation allocates.
class OrderBook {
public:
    explicit OrderBook(const BookConfig& cfg)
        : cfg_(cfg),
          levels_(static_cast<std::size_t>(cfg.max_price - cfg.min_price + 1)),
          bids_(levels_),
          asks_(levels_),
          bid_bits_(levels_),
          ask_bits_(levels_),
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

    // ---- read-only views, used by tests ----
    std::optional<Price> best_bid() const {
        return best_bid_ == kNone ? std::nullopt : std::optional<Price>(price_of(best_bid_));
    }
    std::optional<Price> best_ask() const {
        return best_ask_ == kNone ? std::nullopt : std::optional<Price>(price_of(best_ask_));
    }
    std::size_t order_count() const noexcept { return pool_.in_use(); }
    Qty         depth_at(Side side, Price price) const {
        if (!price_ok(price)) return 0;
        return (side == Side::Buy ? bids_ : asks_)[index_of(price)].total;
    }

private:
    static constexpr std::size_t kNone = LevelBitmap::npos;

    bool        price_ok(Price p) const noexcept { return p >= cfg_.min_price && p <= cfg_.max_price; }
    std::size_t index_of(Price p) const noexcept { return static_cast<std::size_t>(p - cfg_.min_price); }
    Price       price_of(std::size_t i) const noexcept { return cfg_.min_price + static_cast<Price>(i); }

    Order* find(OrderId id) const noexcept {
        return id < cfg_.id_capacity ? index_[id] : nullptr;
    }

    template <EventSink S>
    void execute(const Command& c, S& sink) {
        sink.emit(ev::ack(c));
        Qty remaining = c.qty;
        if (c.side == Side::Buy) {
            match_buy(c, remaining, sink);
        } else {
            match_sell(c, remaining, sink);
        }
        if (remaining == 0) return;
        if (c.order_type == OrderType::Limit) {
            rest(c, remaining);
            sink.emit(ev::rested(c, remaining));
        } else {
            sink.emit(ev::expired(c, remaining));
        }
    }

    // Fill against one level, front to back. Fully filled makers go back to the pool.
    template <EventSink S>
    void sweep_level(PriceLevel& level, Price price, const Command& taker, Qty& remaining, S& sink) {
        Order* maker = level.head;
        while (remaining > 0 && maker != nullptr) {
            const Qty fill = std::min(remaining, maker->qty);
            sink.emit(ev::trade(taker, maker->id, price, fill));
            remaining -= fill;
            maker->qty -= fill;
            level.total -= fill;
            if (maker->qty != 0) break; // taker is exhausted
            Order* next = maker->next;
            level.unlink(maker);
            index_[maker->id] = nullptr;
            pool_.destroy(maker);
            maker = next;
        }
    }

    // A buyer takes from the asks, lowest price first.
    template <EventSink S>
    void match_buy(const Command& c, Qty& remaining, S& sink) {
        const bool market = c.order_type == OrderType::Market;
        while (remaining > 0 && best_ask_ != kNone) {
            const Price price = price_of(best_ask_);
            if (!market && price > c.price) break;
            PriceLevel& level = asks_[best_ask_];
            sweep_level(level, price, c, remaining, sink);
            if (level.empty()) {
                ask_bits_.clear(best_ask_);
                best_ask_ = ask_bits_.next_at_or_above(best_ask_ + 1);
            }
        }
    }

    // A seller takes from the bids, highest price first.
    template <EventSink S>
    void match_sell(const Command& c, Qty& remaining, S& sink) {
        const bool market = c.order_type == OrderType::Market;
        while (remaining > 0 && best_bid_ != kNone) {
            const Price price = price_of(best_bid_);
            if (!market && price < c.price) break;
            PriceLevel& level = bids_[best_bid_];
            sweep_level(level, price, c, remaining, sink);
            if (level.empty()) {
                bid_bits_.clear(best_bid_);
                best_bid_ = best_bid_ == 0 ? kNone : bid_bits_.prev_at_or_below(best_bid_ - 1);
            }
        }
    }

    void rest(const Command& c, Qty remaining) noexcept {
        Order* o      = pool_.create(); // cannot fail: on_new checked pool_.full()
        o->id         = c.order_id;
        o->price      = c.price;
        o->qty        = remaining;
        o->seq        = c.seq;
        o->instrument = c.instrument;
        o->side       = c.side;

        const std::size_t i = index_of(c.price);
        if (c.side == Side::Buy) {
            bids_[i].push_back(o);
            bid_bits_.set(i);
            if (best_bid_ == kNone || i > best_bid_) best_bid_ = i;
        } else {
            asks_[i].push_back(o);
            ask_bits_.set(i);
            if (best_ask_ == kNone || i < best_ask_) best_ask_ = i;
        }
        index_[c.order_id] = o;
    }

    void remove(Order* o) noexcept {
        PriceLevel*       level = o->level;
        const std::size_t i     = index_of(o->price);
        const Side        side  = o->side;
        level->unlink(o);
        if (level->empty()) {
            if (side == Side::Buy) {
                bid_bits_.clear(i);
                if (i == best_bid_) best_bid_ = i == 0 ? kNone : bid_bits_.prev_at_or_below(i - 1);
            } else {
                ask_bits_.clear(i);
                if (i == best_ask_) best_ask_ = ask_bits_.next_at_or_above(i + 1);
            }
        }
        index_[o->id] = nullptr;
        pool_.destroy(o);
    }

    BookConfig              cfg_;
    std::size_t             levels_;
    std::vector<PriceLevel> bids_;
    std::vector<PriceLevel> asks_;
    LevelBitmap             bid_bits_;
    LevelBitmap             ask_bits_;
    std::size_t             best_bid_ = kNone;
    std::size_t             best_ask_ = kNone;
    std::vector<Order*>     index_;
    Pool<Order>             pool_;
};

} // namespace aether
