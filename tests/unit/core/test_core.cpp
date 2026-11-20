#include <gtest/gtest.h>

#include "aether/core/events.hpp"
#include "aether/core/types.hpp"

using namespace aether;

TEST(Types, OppositeSide) {
    EXPECT_EQ(opposite(Side::Buy), Side::Sell);
    EXPECT_EQ(opposite(Side::Sell), Side::Buy);
}

TEST(Types, OppositeIsCompileTime) {
    static_assert(opposite(Side::Buy) == Side::Sell);
    static_assert(opposite(opposite(Side::Sell)) == Side::Sell);
}

TEST(Events, CommandDefaultsAreZero) {
    Command c{};
    EXPECT_EQ(c.seq, 0u);
    EXPECT_EQ(c.price, 0);
    EXPECT_EQ(c.qty, 0);
    EXPECT_EQ(c.type, CommandType::New);
    EXPECT_EQ(c.order_type, OrderType::Limit);
}

TEST(Events, EventDefaultsAreZero) {
    Event e{};
    EXPECT_EQ(e.maker_id, 0u);
    EXPECT_EQ(e.type, EventType::Ack);
    EXPECT_EQ(e.reason, RejectReason::None);
}