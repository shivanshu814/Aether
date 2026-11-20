#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "aether/core/config.hpp"
#include "aether/core/events.hpp"
#include "aether/engine/engine.hpp"
#include "aether/reference/ref_order_book.hpp"

namespace aether::sim {
// Deterministic synthetic order flow: SplitMix64 and integer arithmetic only, so the same
// LoadConfig gives the same commands on every machine and compiler. The generator runs a
// reference engine as it goes, so every cancel and modify names an order that is really
// resting; without that, 90% of them hit filled orders and were rejected for free.
class SplitMix64 {
public:
    explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    constexpr std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z               = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z               = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }

    constexpr std::uint64_t below(std::uint64_t n) noexcept { return next() % n; }

private:
    std::uint64_t state_;
};

struct LoadConfig {
    std::uint64_t seed        = 42;
    std::size_t   count       = 1'000'000;
    std::uint32_t instruments = 1;
    std::size_t   target_live = 20'000;
};

inline BookConfig book_config_for(const LoadConfig& load) {
    BookConfig cfg{};
    cfg.min_price   = 9'000;
    cfg.max_price   = 11'000;
    cfg.max_orders  = load.target_live * 3 + 1024;
    cfg.id_capacity = load.count + 2;
    return cfg;
}

class LiveOrders {
public:
    struct Live {
        OrderId id;
        Price   price;
        Qty     qty;
    };

    explicit LiveOrders(std::uint32_t instruments) : per_instrument_(instruments) {}

    void emit(const Event& e) noexcept {
        Book& book = per_instrument_[e.instrument];
        switch (e.type) {
        case EventType::Rested: book.add({e.order_id, e.price, e.qty}); break;
        case EventType::Canceled: book.remove(e.order_id); break;
        case EventType::Modified: book.set_qty(e.order_id, e.qty); break;
        case EventType::Trade: book.fill(e.maker_id, e.qty); break;
        default: break;
        }
    }

    std::size_t count(InstrumentId i) const { return per_instrument_[i].live.size(); }
    const Live& at(InstrumentId i, std::size_t index) const { return per_instrument_[i].live[index]; }

private:
    struct Book {
        std::vector<Live>                        live;
        std::unordered_map<OrderId, std::size_t> position;

        void add(const Live& o) {
            position[o.id] = live.size();
            live.push_back(o);
        }
        void remove(OrderId id) {
            const auto it = position.find(id);
            if (it == position.end()) return;
            const std::size_t i = it->second;
            position.erase(it);
            if (i != live.size() - 1) {
                live[i]              = live.back();
                position[live[i].id] = i;
            }
            live.pop_back();
        }
        void set_qty(OrderId id, Qty qty) {
            if (const auto it = position.find(id); it != position.end()) live[it->second].qty = qty;
        }
        void fill(OrderId id, Qty qty) {
            const auto it = position.find(id);
            if (it == position.end()) return;
            Live& o = live[it->second];
            o.qty -= qty;
            if (o.qty <= 0) remove(id);
        }
    };

    std::vector<Book> per_instrument_;
};

inline std::vector<Command> generate(const LoadConfig& load, const BookConfig& book) {
    SplitMix64                    rng(load.seed);
    const Price                   centre = (book.min_price + book.max_price) / 2;
    const Price                   margin = 100;
    const std::vector<BookConfig> configs(load.instruments, book);
    Engine<RefOrderBook>          shadow(configs);
    LiveOrders                    live(load.instruments);
    std::vector<Price>            mid(load.instruments, centre);
    std::vector<OrderId>          next_id(load.instruments, 1);

    std::vector<Command> out;
    out.reserve(load.count);

    for (std::size_t n = 0; n < load.count; ++n) {
        Command c{};
        c.seq        = n + 1;
        c.ts_ns      = (n + 1) * 1'000;
        c.instrument = static_cast<InstrumentId>(rng.below(load.instruments));
        const InstrumentId inst = c.instrument;

        const std::uint64_t step = rng.below(2048);
        if (step == 0 && mid[inst] < book.max_price - margin) ++mid[inst];
        if (step == 1 && mid[inst] > book.min_price + margin) --mid[inst];

        const std::size_t   resting      = live.count(inst);
        const std::uint64_t roll         = rng.below(100);
        const std::uint64_t cancel_below = resting > load.target_live ? 60U : 30U;

        if (roll < cancel_below && resting != 0) {
            c.type     = CommandType::Cancel;
            c.order_id = live.at(inst, static_cast<std::size_t>(rng.below(resting))).id;
        } else if (roll < cancel_below + 5 && resting != 0) {
            const LiveOrders::Live o = live.at(inst, static_cast<std::size_t>(rng.below(resting)));
            c.type     = CommandType::Modify;
            c.order_id = o.id;
            if (rng.below(2) == 0 && o.qty > 1) {
                c.price = o.price;
                c.qty   = 1 + static_cast<Qty>(rng.below(static_cast<std::uint64_t>(o.qty - 1)));
            } else {
                c.price = o.price + static_cast<Price>(rng.below(5)) - 2;
                c.qty   = 1 + static_cast<Qty>(rng.below(200));
            }
        } else if (roll < cancel_below + 15) {
            c.type     = CommandType::New;
            c.order_id = next_id[inst]++;
            c.side     = rng.below(2) == 0 ? Side::Buy : Side::Sell;
            c.qty      = 1 + static_cast<Qty>(rng.below(150));
            const std::uint64_t kind = rng.below(4);
            c.order_type = kind < 2 ? OrderType::Limit : kind == 2 ? OrderType::IOC : OrderType::Market;
            if (c.order_type != OrderType::Market) {
                const Price through = 1 + static_cast<Price>(rng.below(5));
                c.price = c.side == Side::Buy ? mid[inst] + through : mid[inst] - through;
            }
        } else {
            c.type       = CommandType::New;
            c.order_id   = next_id[inst]++;
            c.order_type = OrderType::Limit;
            c.side       = rng.below(2) == 0 ? Side::Buy : Side::Sell;
            c.qty        = 1 + static_cast<Qty>(rng.below(100));
            const Price away = 1 + static_cast<Price>(rng.below(10));
            c.price = c.side == Side::Buy ? mid[inst] - away : mid[inst] + away;
        }
        shadow.process(c, live);
        out.push_back(c);
    }
    return out;
}
}
