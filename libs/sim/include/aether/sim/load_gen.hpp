#pragma once

#include <cstdint>
#include <vector>

#include "aether/core/config.hpp"
#include "aether/core/events.hpp"

// Deterministic synthetic order flow. The same LoadConfig always produces the same commands
// on every machine and compiler: the RNG is SplitMix64 and only integer arithmetic is used
// (std:: distributions are not portable across standard libraries).
namespace aether::sim {

class SplitMix64 {
public:
    explicit constexpr SplitMix64(std::uint64_t seed) noexcept : state_(seed) {}

    constexpr std::uint64_t next() noexcept {
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
        z               = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z               = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
    // Uniform-ish integer in [0, n). The tiny modulo bias does not matter here.
    constexpr std::uint64_t below(std::uint64_t n) noexcept { return next() % n; }

private:
    std::uint64_t state_;
};

struct LoadConfig {
    std::uint64_t seed        = 42;
    std::size_t   count       = 1'000'000; // commands to generate
    std::uint32_t instruments = 1;
    std::size_t   target_live = 20'000;    // per instrument; above this, cancels are favoured
};

// Book limits that fit a stream generated from `load`.
inline BookConfig book_config_for(const LoadConfig& load) {
    BookConfig cfg{};
    cfg.min_price   = 9'000;
    cfg.max_price   = 11'000;
    cfg.max_orders  = load.target_live * 3 + 1024;
    cfg.id_capacity = load.count + 2; // ids are per instrument and never exceed count
    return cfg;
}

// Mix: ~55% passive limit orders around a slowly moving mid price, ~30% cancels,
// ~10% aggressive orders (crossing limit, IOC, market) and ~5% modifies.
// seq runs 1..count; ts_ns is a synthetic clock that advances 1 microsecond per command.
inline std::vector<Command> generate(const LoadConfig& load, const BookConfig& book) {
    struct Live {
        OrderId id;
        Price   price;
        Qty     qty;
    };
    struct Instrument {
        Price             mid;
        OrderId           next_id = 1;
        std::vector<Live> live; // orders this generator believes are resting
    };

    SplitMix64              rng(load.seed);
    const Price             centre = (book.min_price + book.max_price) / 2;
    const Price             margin = 100;
    std::vector<Instrument> instruments(load.instruments);
    for (Instrument& inst : instruments) {
        inst.mid = centre;
        inst.live.reserve(load.target_live * 2);
    }

    std::vector<Command> out;
    out.reserve(load.count);

    for (std::size_t n = 0; n < load.count; ++n) {
        Command c{};
        c.seq        = n + 1;
        c.ts_ns      = (n + 1) * 1'000;
        c.instrument = static_cast<InstrumentId>(rng.below(load.instruments));
        Instrument& inst = instruments[c.instrument];

        // Random walk of the mid price, kept away from the edges of the band.
        const std::uint64_t step = rng.below(16);
        if (step == 0 && inst.mid < book.max_price - margin) ++inst.mid;
        if (step == 1 && inst.mid > book.min_price + margin) --inst.mid;

        const bool          crowded = inst.live.size() > load.target_live;
        const std::uint64_t roll    = rng.below(100);
        const std::uint64_t cancel_below = crowded ? 60U : 30U;

        if (roll < cancel_below && !inst.live.empty()) {
            // Cancel a random order we believe is live (it may already have been filled).
            const std::size_t i = static_cast<std::size_t>(rng.below(inst.live.size()));
            c.type     = CommandType::Cancel;
            c.order_id = inst.live[i].id;
            inst.live[i] = inst.live.back();
            inst.live.pop_back();
        } else if (roll < cancel_below + 5 && !inst.live.empty()) {
            const std::size_t i = static_cast<std::size_t>(rng.below(inst.live.size()));
            Live&             o = inst.live[i];
            c.type     = CommandType::Modify;
            c.order_id = o.id;
            if (rng.below(2) == 0 && o.qty > 1) {
                c.price = o.price; // qty down, same price: keeps priority
                c.qty   = 1 + static_cast<Qty>(rng.below(static_cast<std::uint64_t>(o.qty - 1)));
            } else {
                c.price = o.price + static_cast<Price>(rng.below(5)) - 2; // re-price: loses priority
                c.qty   = 1 + static_cast<Qty>(rng.below(200));
            }
            o.price = c.price;
            o.qty   = c.qty;
        } else if (roll < cancel_below + 15) {
            // Aggressive: crosses the spread.
            c.type     = CommandType::New;
            c.order_id = inst.next_id++;
            c.side     = rng.below(2) == 0 ? Side::Buy : Side::Sell;
            c.qty      = 1 + static_cast<Qty>(rng.below(500));
            const std::uint64_t kind = rng.below(4);
            c.order_type = kind < 2 ? OrderType::Limit : kind == 2 ? OrderType::IOC : OrderType::Market;
            if (c.order_type != OrderType::Market) {
                const Price through = 1 + static_cast<Price>(rng.below(5));
                c.price = c.side == Side::Buy ? inst.mid + through : inst.mid - through;
            }
            if (c.order_type == OrderType::Limit) inst.live.push_back({c.order_id, c.price, c.qty});
        } else {
            // Passive limit order a few ticks away from the mid.
            c.type       = CommandType::New;
            c.order_id   = inst.next_id++;
            c.order_type = OrderType::Limit;
            c.side       = rng.below(2) == 0 ? Side::Buy : Side::Sell;
            c.qty        = 1 + static_cast<Qty>(rng.below(100));
            const Price away = 1 + static_cast<Price>(rng.below(10));
            c.price = c.side == Side::Buy ? inst.mid - away : inst.mid + away;
            inst.live.push_back({c.order_id, c.price, c.qty});
        }
        out.push_back(c);
    }
    return out;
}

} // namespace aether::sim
