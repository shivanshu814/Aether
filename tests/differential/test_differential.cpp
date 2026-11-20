// Differential test: the same seeded command stream goes through the reference engine and
// the fast engine. After every single command their events must be identical, and at the end
// the visible book state must match. A failure prints the seed and command index, which
// reproduces the bug exactly.
#include <gtest/gtest.h>

#include <vector>

#include "aether/book/order_book.hpp"
#include "aether/engine/engine.hpp"
#include "aether/reference/ref_order_book.hpp"
#include "aether/sim/load_gen.hpp"
#include "support/vector_sink.hpp"

using namespace aether;

namespace {

void run_differential(const sim::LoadConfig& load, const BookConfig& book, bool expect_rejects) {
    // Generate against the wide default band; `book` may be tighter to provoke rejects.
    const std::vector<Command>    commands = sim::generate(load, sim::book_config_for(load));
    const std::vector<BookConfig> instruments(load.instruments, book);

    Engine<RefOrderBook> reference(instruments);
    Engine<OrderBook>    fast(instruments);
    test::VectorSink     expected;
    test::VectorSink     actual;
    std::uint64_t        trades = 0, rejects = 0;

    for (std::size_t i = 0; i < commands.size(); ++i) {
        expected.clear();
        actual.clear();
        reference.process(commands[i], expected);
        fast.process(commands[i], actual);
        ASSERT_EQ(actual.events, expected.events)
            << "seed=" << load.seed << " command index=" << i << " seq=" << commands[i].seq;
        for (const Event& e : expected.events) {
            trades += e.type == EventType::Trade;
            rejects += e.type == EventType::Reject;
        }
    }

    for (InstrumentId id = 0; id < load.instruments; ++id) {
        EXPECT_EQ(fast.book(id).best_bid(), reference.book(id).best_bid()) << "instrument " << id;
        EXPECT_EQ(fast.book(id).best_ask(), reference.book(id).best_ask()) << "instrument " << id;
        EXPECT_EQ(fast.book(id).order_count(), reference.book(id).order_count()) << "instrument " << id;
        for (Price p = book.min_price; p <= book.max_price; ++p) {
            ASSERT_EQ(fast.book(id).depth_at(Side::Buy, p), reference.book(id).depth_at(Side::Buy, p));
            ASSERT_EQ(fast.book(id).depth_at(Side::Sell, p), reference.book(id).depth_at(Side::Sell, p));
        }
    }
    // Guard against a stream that never exercises matching at all.
    EXPECT_GT(trades, expect_rejects ? std::uint64_t{100} : commands.size() / 50)
        << "stream produced too few trades to be a useful test";
    if (expect_rejects) {
        EXPECT_GT(rejects, commands.size() / 50) << "tight limits should have produced rejects";
    }
}

// The generator must produce work that reaches the book: with matching limits, every cancel
// and modify names an order that is resting at that moment, so nothing is rejected, and the
// book settles near its target size.
TEST(LoadGenerator, CancelsAndModifiesAlwaysHitRestingOrders) {
    sim::LoadConfig load{};
    load.seed        = 99;
    load.count       = 200'000;
    load.instruments = 2;
    load.target_live = 1'000;
    const BookConfig              book = sim::book_config_for(load);
    const std::vector<Command>    commands = sim::generate(load, book);
    const std::vector<BookConfig> instruments(load.instruments, book);

    Engine<OrderBook> engine(instruments);
    test::VectorSink  sink;
    std::uint64_t     rejects = 0, cancels = 0, canceled = 0, trades = 0;
    for (const Command& c : commands) {
        sink.clear();
        engine.process(c, sink);
        cancels += c.type == CommandType::Cancel;
        for (const Event& e : sink.events) {
            rejects += e.type == EventType::Reject;
            trades += e.type == EventType::Trade;
            canceled += e.type == EventType::Canceled && c.type == CommandType::Cancel;
        }
    }
    EXPECT_EQ(rejects, 0u);
    EXPECT_EQ(canceled, cancels);          // every cancel removed a real order
    EXPECT_GT(cancels, commands.size() / 5);
    EXPECT_GT(trades, commands.size() / 10);
    for (InstrumentId id = 0; id < load.instruments; ++id) {
        EXPECT_GT(engine.book(id).order_count(), 800u);
        EXPECT_LT(engine.book(id).order_count(), 1'200u);
    }
    EXPECT_EQ(sim::generate(load, book), commands); // same config, same stream
}

class Differential : public ::testing::TestWithParam<std::uint64_t> {};

TEST_P(Differential, FastEngineMatchesReference) {
    sim::LoadConfig load{};
    load.seed        = GetParam();
    load.count       = 150'000;
    load.instruments = 2;
    load.target_live = 2'000;
    run_differential(load, sim::book_config_for(load), false);
}

// A tiny book: narrow price band and very few order slots, so InvalidPrice and BookFull
// rejects, pool exhaustion and constant level creation/removal are all exercised.
TEST_P(Differential, TightLimitsProvokeRejects) {
    sim::LoadConfig load{};
    load.seed        = GetParam() + 1'000;
    load.count       = 100'000;
    load.instruments = 1;
    load.target_live = 500;

    BookConfig tight = sim::book_config_for(load);
    tight.min_price  = 9'950;
    tight.max_price  = 10'050;
    tight.max_orders = 64;
    run_differential(load, tight, true);
}

INSTANTIATE_TEST_SUITE_P(Seeds, Differential, ::testing::Values(1, 2, 3, 4, 5, 6));

} // namespace
