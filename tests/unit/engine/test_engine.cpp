#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <vector>

#include "aether/book/order_book.hpp"
#include "aether/engine/engine.hpp"
#include "aether/reference/ref_order_book.hpp"
#include "aether/sim/load_gen.hpp"
#include "support/vector_sink.hpp"

using namespace aether;

namespace {

Command new_limit(SeqNo seq, InstrumentId instrument, OrderId id, Side side, Price price, Qty qty) {
    Command c{};
    c.seq        = seq;
    c.instrument = instrument;
    c.order_id   = id;
    c.side       = side;
    c.price      = price;
    c.qty        = qty;
    return c;
}

template <class Book>
class EngineTest : public ::testing::Test {};
using BookTypes = ::testing::Types<RefOrderBook, OrderBook>;
TYPED_TEST_SUITE(EngineTest, BookTypes);

TYPED_TEST(EngineTest, UnknownInstrumentIsRejected) {
    const std::vector<BookConfig> instruments(2);
    Engine<TypeParam>             engine(instruments);
    test::VectorSink              sink;
    engine.process(new_limit(1, 2, 1, Side::Buy, 100, 10), sink);
    ASSERT_EQ(sink.events.size(), 1u);
    EXPECT_EQ(sink.events[0].type, EventType::Reject);
    EXPECT_EQ(sink.events[0].reason, RejectReason::UnknownInstrument);
    EXPECT_EQ(sink.events[0].instrument, 2u);
}

TYPED_TEST(EngineTest, InstrumentsNeverMatchEachOther) {
    const std::vector<BookConfig> instruments(2);
    Engine<TypeParam>             engine(instruments);
    test::VectorSink              sink;
    engine.process(new_limit(1, 0, 1, Side::Sell, 100, 10), sink);
    engine.process(new_limit(2, 1, 1, Side::Buy, 100, 10), sink); // same price, other instrument
    for (const Event& e : sink.events) EXPECT_NE(e.type, EventType::Trade);
    EXPECT_EQ(engine.book(0).best_ask(), 100);
    EXPECT_EQ(engine.book(1).best_bid(), 100);

    sink.clear();
    engine.process(new_limit(3, 0, 2, Side::Buy, 100, 10), sink); // now the same instrument
    ASSERT_EQ(sink.events.size(), 2u);
    EXPECT_EQ(sink.events[1].type, EventType::Trade);
    EXPECT_EQ(sink.events[1].instrument, 0u);
}

TYPED_TEST(EngineTest, ProcessBatchMatchesProcessOneByOne) {
    sim::LoadConfig load{};
    load.seed        = 5;
    load.count       = 50'000;
    load.instruments = 3;
    load.target_live = 500;
    const BookConfig              book = sim::book_config_for(load);
    const std::vector<Command>    commands = sim::generate(load, book);
    const std::vector<BookConfig> instruments(load.instruments, book);

    Engine<TypeParam> one_by_one(instruments);
    Engine<TypeParam> batched(instruments);
    test::VectorSink  expected;
    test::VectorSink  actual;
    for (const Command& c : commands) one_by_one.process(c, expected);
    // Uneven batches, including ones shorter than the prefetch distance.
    std::size_t i = 0;
    for (std::size_t len = 1; i < commands.size(); len = len * 3 % 701 + 1) {
        const std::size_t n = std::min(len, commands.size() - i);
        batched.process_batch(std::span<const Command>(commands.data() + i, n), actual);
        i += n;
    }
    EXPECT_EQ(actual.events, expected.events);
}

} // namespace
