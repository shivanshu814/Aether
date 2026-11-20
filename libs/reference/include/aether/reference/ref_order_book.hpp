#pragma once

#include <algorithm>
#include <deque>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>

#include "aether/core/config.hpp"
#include "aether/core/event_factory.hpp"
#include "aether/core/event_sink.hpp"

namespace aether {

// Reference order book: slow, simple and meant to be obviously correct.
// It is the oracle the fast OrderBook is checked against, so it must never be optimised.
// Everything is done with standard containers:
//   price levels  -> std::map (sorted), one per side
//   FIFO at price -> std::deque
//   id lookup     -> std::unordered_map
class RefOrderBook {
public:
    explicit RefOrderBook(const BookConfig& cfg) : cfg_(cfg) {}

    template <EventSink S>
    void on_new(const Command& c, S& sink) {
        if (c.qty <= 0) {
            sink.emit(ev::reject(c, RejectReason::InvalidQty));
            return;
        }
        if (c.order_type != OrderType::Market && !price_ok(c.price)) {
            sink.emit(ev::reject(c, RejectReason::InvalidPrice));
            return;
        }
        if (c.order_id >= cfg_.id_capacity) {
            sink.emit(ev::reject(c, RejectReason::InvalidOrderId));
            return;
        }
        if (index_.contains(c.order_id)) {
            sink.emit(ev::reject(c, RejectReason::DuplicateOrder));
            return;
        }
        if (c.order_type == OrderType::Limit && index_.size() >= cfg_.max_orders) {
            sink.emit(ev::reject(c, RejectReason::BookFull));
            return;
        }
        execute(c, sink);
    }

    template <EventSink S>
    void on_cancel(const Command& c, S& sink) {
        const auto it = index_.find(c.order_id);
        if (it == index_.end()) {
            sink.emit(ev::reject(c, RejectReason::UnknownOrder));
            return;
        }
        const Info info = it->second;
        const Qty  left = remove(c.order_id, info);
        sink.emit(ev::canceled(c, info.side, info.price, left));
    }

    // Same price and qty not above what is left: change in place, priority kept.
    // Anything else (price change or qty up): cancel, then re-enter as a new limit order.
    template <EventSink S>
    void on_modify(const Command& c, S& sink) {
        if (c.qty <= 0) {
            sink.emit(ev::reject(c, RejectReason::InvalidQty));
            return;
        }
        const auto it = index_.find(c.order_id);
        if (it == index_.end()) {
            sink.emit(ev::reject(c, RejectReason::UnknownOrder));
            return;
        }
        if (!price_ok(c.price)) {
            sink.emit(ev::reject(c, RejectReason::InvalidPrice));
            return;
        }
        const Info info = it->second;
        RefOrder&  o    = find_order(c.order_id, info);
        if (c.price == info.price && c.qty <= o.qty) {
            o.qty = c.qty;
            sink.emit(ev::modified(c, info.side, info.price, c.qty));
            return;
        }
        const Qty left = remove(c.order_id, info);
        sink.emit(ev::canceled(c, info.side, info.price, left));

        Command fresh    = c;
        fresh.type       = CommandType::New;
        fresh.side       = info.side;
        fresh.order_type = OrderType::Limit;
        execute(fresh, sink);
    }

    // ---- read-only views, used by tests ----
    std::optional<Price> best_bid() const {
        return bids_.empty() ? std::nullopt : std::optional<Price>(bids_.begin()->first);
    }
    std::optional<Price> best_ask() const {
        return asks_.empty() ? std::nullopt : std::optional<Price>(asks_.begin()->first);
    }
    std::size_t order_count() const { return index_.size(); }
    Qty         depth_at(Side side, Price price) const {
        Qty total = 0;
        if (side == Side::Buy) {
            if (const auto it = bids_.find(price); it != bids_.end())
                for (const RefOrder& o : it->second) total += o.qty;
        } else {
            if (const auto it = asks_.find(price); it != asks_.end())
                for (const RefOrder& o : it->second) total += o.qty;
        }
        return total;
    }

private:
    struct RefOrder {
        OrderId id;
        Qty     qty;
    };
    struct Info {
        Side  side;
        Price price;
    };
    using Queue = std::deque<RefOrder>;
    using Bids  = std::map<Price, Queue, std::greater<Price>>; // best (highest) first
    using Asks  = std::map<Price, Queue, std::less<Price>>;    // best (lowest) first

    bool price_ok(Price p) const { return p >= cfg_.min_price && p <= cfg_.max_price; }

    static bool crosses(const Command& taker, Price level_price) {
        if (taker.order_type == OrderType::Market) return true;
        return taker.side == Side::Buy ? level_price <= taker.price : level_price >= taker.price;
    }

    // Ack, match against the opposite side, then rest or expire what is left.
    template <EventSink S>
    void execute(const Command& c, S& sink) {
        sink.emit(ev::ack(c));
        Qty remaining = c.qty;
        if (c.side == Side::Buy) {
            match(asks_, c, remaining, sink);
        } else {
            match(bids_, c, remaining, sink);
        }
        if (remaining == 0) return;
        if (c.order_type == OrderType::Limit) {
            if (c.side == Side::Buy) {
                bids_[c.price].push_back({c.order_id, remaining});
            } else {
                asks_[c.price].push_back({c.order_id, remaining});
            }
            index_.emplace(c.order_id, Info{c.side, c.price});
            sink.emit(ev::rested(c, remaining));
        } else {
            sink.emit(ev::expired(c, remaining));
        }
    }

    template <class Levels, EventSink S>
    void match(Levels& opposite_side, const Command& taker, Qty& remaining, S& sink) {
        while (remaining > 0 && !opposite_side.empty()) {
            const auto  best  = opposite_side.begin();
            const Price price = best->first;
            if (!crosses(taker, price)) break;
            Queue& queue = best->second;
            while (remaining > 0 && !queue.empty()) {
                RefOrder& maker = queue.front(); // time priority: oldest first
                const Qty fill  = std::min(remaining, maker.qty);
                sink.emit(ev::trade(taker, maker.id, price, fill));
                remaining -= fill;
                maker.qty -= fill;
                if (maker.qty == 0) {
                    index_.erase(maker.id);
                    queue.pop_front();
                }
            }
            if (queue.empty()) opposite_side.erase(best);
        }
    }

    template <class Levels>
    static RefOrder& find_in(Levels& levels, OrderId id, Price price) {
        Queue& queue = levels.find(price)->second;
        return *std::find_if(queue.begin(), queue.end(),
                             [id](const RefOrder& o) { return o.id == id; });
    }

    RefOrder& find_order(OrderId id, Info info) {
        return info.side == Side::Buy ? find_in(bids_, id, info.price)
                                      : find_in(asks_, id, info.price);
    }

    template <class Levels>
    static Qty remove_from(Levels& levels, OrderId id, Price price) {
        const auto level = levels.find(price);
        Queue&     queue = level->second;
        const auto it    = std::find_if(queue.begin(), queue.end(),
                                        [id](const RefOrder& o) { return o.id == id; });
        const Qty  left  = it->qty;
        queue.erase(it);
        if (queue.empty()) levels.erase(level);
        return left;
    }

    // Removes the order from its level and the index. Returns the qty it still had.
    Qty remove(OrderId id, Info info) {
        const Qty left = info.side == Side::Buy ? remove_from(bids_, id, info.price)
                                                : remove_from(asks_, id, info.price);
        index_.erase(id);
        return left;
    }

    BookConfig                        cfg_;
    Bids                              bids_;
    Asks                              asks_;
    std::unordered_map<OrderId, Info> index_;
};

} // namespace aether
