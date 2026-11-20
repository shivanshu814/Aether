// Hand-written matching scenarios. Every test runs against BOTH books, so the reference
// (oracle) and the fast book are held to the same expected behaviour.
#include <gtest/gtest.h>

#include <vector>

#include "aether/book/order_book.hpp"
#include "aether/reference/ref_order_book.hpp"
#include "support/vector_sink.hpp"

using namespace aether;

namespace {

BookConfig small_config() {
    BookConfig cfg{};
    cfg.min_price   = 10'000;
    cfg.max_price   = 10'200;
    cfg.max_orders  = 16;
    cfg.id_capacity = 1'000;
    return cfg;
}

Event make(EventType type, SeqNo seq, OrderId id, Side side, Price price, Qty qty,
           OrderId maker = 0, RejectReason reason = RejectReason::None) {
    Event e{};
    e.type     = type;
    e.seq      = seq;
    e.order_id = id;
    e.side     = side;
    e.price    = price;
    e.qty      = qty;
    e.maker_id = maker;
    e.reason   = reason;
    return e;
}

template <class Book>
class Matching : public ::testing::Test {
protected:
    Book             book{small_config()};
    test::VectorSink sink;
    SeqNo            seq = 0;

    Command command(CommandType type, OrderId id, Side side, OrderType order_type, Price price, Qty qty) {
        Command c{};
        c.seq        = ++seq;
        c.type       = type;
        c.order_id   = id;
        c.side       = side;
        c.order_type = order_type;
        c.price      = price;
        c.qty        = qty;
        return c;
    }
    void limit(OrderId id, Side side, Price price, Qty qty) {
        book.on_new(command(CommandType::New, id, side, OrderType::Limit, price, qty), sink);
    }
    void ioc(OrderId id, Side side, Price price, Qty qty) {
        book.on_new(command(CommandType::New, id, side, OrderType::IOC, price, qty), sink);
    }
    void market(OrderId id, Side side, Qty qty) {
        book.on_new(command(CommandType::New, id, side, OrderType::Market, 0, qty), sink);
    }
    void cancel(OrderId id) {
        book.on_cancel(command(CommandType::Cancel, id, Side::Buy, OrderType::Limit, 0, 0), sink);
    }
    void modify(OrderId id, Price price, Qty qty) {
        book.on_modify(command(CommandType::Modify, id, Side::Buy, OrderType::Limit, price, qty), sink);
    }

    // The book from the design document's walkthrough (prices in ticks of 0.01).
    void build_walkthrough_book() {
        limit(1, Side::Buy, 10'099, 300);
        limit(2, Side::Sell, 10'101, 50);
        limit(3, Side::Buy, 10'098, 500);
        limit(4, Side::Sell, 10'102, 100);
        limit(5, Side::Sell, 10'101, 150);
        limit(6, Side::Buy, 10'099, 100);
        limit(7, Side::Sell, 10'102, 300);
        limit(8, Side::Sell, 10'101, 100);
        limit(9, Side::Sell, 10'103, 200);
        limit(10, Side::Buy, 10'097, 250);
        sink.clear();
    }
};

using BookTypes = ::testing::Types<RefOrderBook, OrderBook>;
TYPED_TEST_SUITE(Matching, BookTypes);

TYPED_TEST(Matching, LimitOrderRestsOnEmptyBook) {
    this->limit(1, Side::Buy, 10'100, 100);
    const std::vector<Event> expected = {
        make(EventType::Ack, 1, 1, Side::Buy, 10'100, 100),
        make(EventType::Rested, 1, 1, Side::Buy, 10'100, 100),
    };
    EXPECT_EQ(this->sink.events, expected);
    EXPECT_EQ(this->book.best_bid(), 10'100);
    EXPECT_EQ(this->book.best_ask(), std::nullopt);
    EXPECT_EQ(this->book.order_count(), 1u);
}

TYPED_TEST(Matching, WalkthroughBuy450SweepsFiveMakers) {
    this->build_walkthrough_book();
    this->limit(11, Side::Buy, 10'102, 450);
    const std::vector<Event> expected = {
        make(EventType::Ack, 11, 11, Side::Buy, 10'102, 450),
        make(EventType::Trade, 11, 11, Side::Buy, 10'101, 50, 2),
        make(EventType::Trade, 11, 11, Side::Buy, 10'101, 150, 5),
        make(EventType::Trade, 11, 11, Side::Buy, 10'101, 100, 8),
        make(EventType::Trade, 11, 11, Side::Buy, 10'102, 100, 4),
        make(EventType::Trade, 11, 11, Side::Buy, 10'102, 50, 7),
    };
    EXPECT_EQ(this->sink.events, expected);
    EXPECT_EQ(this->book.best_ask(), 10'102);               // 101.01 is gone
    EXPECT_EQ(this->book.depth_at(Side::Sell, 10'102), 250); // #7 keeps 250
    EXPECT_EQ(this->book.best_bid(), 10'099);                // nothing rested
    EXPECT_EQ(this->book.order_count(), 6u);
}

TYPED_TEST(Matching, WalkthroughBuy800RestsTheLeftover) {
    this->build_walkthrough_book();
    this->limit(11, Side::Buy, 10'102, 800);
    ASSERT_EQ(this->sink.events.size(), 7u); // Ack, 5 trades, Rested
    EXPECT_EQ(this->sink.events.back(), make(EventType::Rested, 11, 11, Side::Buy, 10'102, 100));
    EXPECT_EQ(this->book.best_bid(), 10'102);
    EXPECT_EQ(this->book.best_ask(), 10'103);
}

TYPED_TEST(Matching, TradePrintsAtMakerPrice) {
    this->limit(1, Side::Sell, 10'100, 10);
    this->sink.clear();
    this->limit(2, Side::Buy, 10'150, 10); // willing to pay more, still trades at 10'100
    ASSERT_EQ(this->sink.events.size(), 2u);
    EXPECT_EQ(this->sink.events[1], make(EventType::Trade, 2, 2, Side::Buy, 10'100, 10, 1));
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(Matching, SellSweepsBidsHighestFirst) {
    this->limit(1, Side::Buy, 10'098, 10);
    this->limit(2, Side::Buy, 10'100, 10);
    this->limit(3, Side::Buy, 10'099, 10);
    this->sink.clear();
    this->limit(4, Side::Sell, 10'099, 25);
    const std::vector<Event> expected = {
        make(EventType::Ack, 4, 4, Side::Sell, 10'099, 25),
        make(EventType::Trade, 4, 4, Side::Sell, 10'100, 10, 2),
        make(EventType::Trade, 4, 4, Side::Sell, 10'099, 10, 3),
        make(EventType::Rested, 4, 4, Side::Sell, 10'099, 5),
    };
    EXPECT_EQ(this->sink.events, expected);
    EXPECT_EQ(this->book.best_bid(), 10'098);
    EXPECT_EQ(this->book.best_ask(), 10'099);
}

TYPED_TEST(Matching, MarketOrderOnEmptyBookExpires) {
    this->market(1, Side::Buy, 100);
    const std::vector<Event> expected = {
        make(EventType::Ack, 1, 1, Side::Buy, 0, 100),
        make(EventType::Expired, 1, 1, Side::Buy, 0, 100),
    };
    EXPECT_EQ(this->sink.events, expected);
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(Matching, MarketOrderIgnoresPriceAndNeverRests) {
    this->limit(1, Side::Sell, 10'150, 30);
    this->limit(2, Side::Sell, 10'190, 30);
    this->sink.clear();
    this->market(3, Side::Buy, 100);
    const std::vector<Event> expected = {
        make(EventType::Ack, 3, 3, Side::Buy, 0, 100),
        make(EventType::Trade, 3, 3, Side::Buy, 10'150, 30, 1),
        make(EventType::Trade, 3, 3, Side::Buy, 10'190, 30, 2),
        make(EventType::Expired, 3, 3, Side::Buy, 0, 40),
    };
    EXPECT_EQ(this->sink.events, expected);
    EXPECT_EQ(this->book.best_bid(), std::nullopt);
}

TYPED_TEST(Matching, IocFillsWhatItCanThenExpires) {
    this->limit(1, Side::Sell, 10'100, 30);
    this->limit(2, Side::Sell, 10'105, 30);
    this->sink.clear();
    this->ioc(3, Side::Buy, 10'100, 50);
    const std::vector<Event> expected = {
        make(EventType::Ack, 3, 3, Side::Buy, 10'100, 50),
        make(EventType::Trade, 3, 3, Side::Buy, 10'100, 30, 1),
        make(EventType::Expired, 3, 3, Side::Buy, 0, 20),
    };
    EXPECT_EQ(this->sink.events, expected);
    EXPECT_EQ(this->book.best_bid(), std::nullopt); // IOC never rests
    EXPECT_EQ(this->book.best_ask(), 10'105);
}

TYPED_TEST(Matching, CancelRemovesOrderAndRepairsBestPrice) {
    this->limit(1, Side::Buy, 10'100, 10);
    this->limit(2, Side::Buy, 10'090, 20);
    this->sink.clear();
    this->cancel(1);
    const std::vector<Event> expected = {make(EventType::Canceled, 3, 1, Side::Buy, 10'100, 10)};
    EXPECT_EQ(this->sink.events, expected);
    EXPECT_EQ(this->book.best_bid(), 10'090);
    EXPECT_EQ(this->book.order_count(), 1u);
}

TYPED_TEST(Matching, CancelMiddleOfQueueKeepsOthersInOrder) {
    this->limit(1, Side::Sell, 10'100, 10);
    this->limit(2, Side::Sell, 10'100, 20);
    this->limit(3, Side::Sell, 10'100, 30);
    this->cancel(2);
    this->sink.clear();
    this->limit(4, Side::Buy, 10'100, 40);
    const std::vector<Event> expected = {
        make(EventType::Ack, 5, 4, Side::Buy, 10'100, 40),
        make(EventType::Trade, 5, 4, Side::Buy, 10'100, 10, 1),
        make(EventType::Trade, 5, 4, Side::Buy, 10'100, 30, 3),
    };
    EXPECT_EQ(this->sink.events, expected);
}

TYPED_TEST(Matching, CancelUnknownOrFilledOrderIsRejected) {
    this->cancel(99);
    EXPECT_EQ(this->sink.events.back(),
              make(EventType::Reject, 1, 99, Side::Buy, 0, 0, 0, RejectReason::UnknownOrder));
    this->limit(1, Side::Sell, 10'100, 10);
    this->limit(2, Side::Buy, 10'100, 10); // fills #1 completely
    this->cancel(1);
    EXPECT_EQ(this->sink.events.back().reason, RejectReason::UnknownOrder);
}

TYPED_TEST(Matching, InvalidOrdersAreRejectedWithoutAck) {
    this->limit(1, Side::Buy, 10'100, 0);
    this->limit(2, Side::Buy, 9'999, 10);   // below the band
    this->limit(3, Side::Buy, 10'201, 10);  // above the band
    this->limit(5'000, Side::Buy, 10'100, 10); // id outside id_capacity
    ASSERT_EQ(this->sink.events.size(), 4u);
    EXPECT_EQ(this->sink.events[0].reason, RejectReason::InvalidQty);
    EXPECT_EQ(this->sink.events[1].reason, RejectReason::InvalidPrice);
    EXPECT_EQ(this->sink.events[2].reason, RejectReason::InvalidPrice);
    EXPECT_EQ(this->sink.events[3].reason, RejectReason::InvalidOrderId);
    for (const Event& e : this->sink.events) EXPECT_EQ(e.type, EventType::Reject);
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(Matching, DuplicateLiveOrderIdIsRejected) {
    this->limit(1, Side::Buy, 10'100, 10);
    this->sink.clear();
    this->limit(1, Side::Sell, 10'150, 10);
    ASSERT_EQ(this->sink.events.size(), 1u);
    EXPECT_EQ(this->sink.events[0].reason, RejectReason::DuplicateOrder);
    EXPECT_EQ(this->book.order_count(), 1u);
}

TYPED_TEST(Matching, BookFullRejectsNewLimitOrders) {
    for (OrderId id = 1; id <= 16; ++id) this->limit(id, Side::Buy, 10'000 + static_cast<Price>(id), 1);
    EXPECT_EQ(this->book.order_count(), 16u);
    this->sink.clear();
    this->limit(17, Side::Buy, 10'050, 1);
    ASSERT_EQ(this->sink.events.size(), 1u);
    EXPECT_EQ(this->sink.events[0].reason, RejectReason::BookFull);
    this->cancel(1); // frees a slot
    this->sink.clear();
    this->limit(18, Side::Buy, 10'050, 1);
    EXPECT_EQ(this->sink.events.back().type, EventType::Rested);
}

TYPED_TEST(Matching, ModifyQtyDownKeepsTimePriority) {
    this->limit(1, Side::Sell, 10'100, 50);
    this->limit(2, Side::Sell, 10'100, 50);
    this->sink.clear();
    this->modify(1, 10'100, 20);
    const std::vector<Event> modified = {make(EventType::Modified, 3, 1, Side::Sell, 10'100, 20)};
    EXPECT_EQ(this->sink.events, modified);
    EXPECT_EQ(this->book.depth_at(Side::Sell, 10'100), 70);
    this->sink.clear();
    this->limit(3, Side::Buy, 10'100, 30);
    const std::vector<Event> expected = {
        make(EventType::Ack, 4, 3, Side::Buy, 10'100, 30),
        make(EventType::Trade, 4, 3, Side::Buy, 10'100, 20, 1), // #1 is still first
        make(EventType::Trade, 4, 3, Side::Buy, 10'100, 10, 2),
    };
    EXPECT_EQ(this->sink.events, expected);
}

TYPED_TEST(Matching, ModifyQtyUpLosesTimePriority) {
    this->limit(1, Side::Sell, 10'100, 50);
    this->limit(2, Side::Sell, 10'100, 50);
    this->sink.clear();
    this->modify(1, 10'100, 80);
    const std::vector<Event> replaced = {
        make(EventType::Canceled, 3, 1, Side::Sell, 10'100, 50),
        make(EventType::Ack, 3, 1, Side::Sell, 10'100, 80),
        make(EventType::Rested, 3, 1, Side::Sell, 10'100, 80),
    };
    EXPECT_EQ(this->sink.events, replaced);
    this->sink.clear();
    this->limit(3, Side::Buy, 10'100, 60);
    ASSERT_EQ(this->sink.events.size(), 3u);
    EXPECT_EQ(this->sink.events[1].maker_id, 2u); // #2 is now ahead of #1
    EXPECT_EQ(this->sink.events[2].maker_id, 1u);
}

TYPED_TEST(Matching, ModifyPriceCanCrossAndTrade) {
    this->limit(1, Side::Sell, 10'110, 10);
    this->limit(2, Side::Buy, 10'100, 10);
    this->sink.clear();
    this->modify(2, 10'110, 10); // re-priced up to the ask
    const std::vector<Event> expected = {
        make(EventType::Canceled, 3, 2, Side::Buy, 10'100, 10),
        make(EventType::Ack, 3, 2, Side::Buy, 10'110, 10),
        make(EventType::Trade, 3, 2, Side::Buy, 10'110, 10, 1),
    };
    EXPECT_EQ(this->sink.events, expected);
    EXPECT_EQ(this->book.order_count(), 0u);
}

TYPED_TEST(Matching, ModifyRejectsLeaveTheOrderUntouched) {
    this->limit(1, Side::Buy, 10'100, 10);
    this->sink.clear();
    this->modify(1, 10'100, 0);
    this->modify(1, 9'000, 5);
    this->modify(77, 10'100, 5);
    ASSERT_EQ(this->sink.events.size(), 3u);
    EXPECT_EQ(this->sink.events[0].reason, RejectReason::InvalidQty);
    EXPECT_EQ(this->sink.events[1].reason, RejectReason::InvalidPrice);
    EXPECT_EQ(this->sink.events[2].reason, RejectReason::UnknownOrder);
    EXPECT_EQ(this->book.depth_at(Side::Buy, 10'100), 10);
}

TYPED_TEST(Matching, OrderIdCanBeReusedAfterItLeavesTheBook) {
    this->limit(1, Side::Buy, 10'100, 10);
    this->cancel(1);
    this->sink.clear();
    this->limit(1, Side::Sell, 10'150, 5);
    EXPECT_EQ(this->sink.events.back(), make(EventType::Rested, 3, 1, Side::Sell, 10'150, 5));
}

} // namespace
