#include <gtest/gtest.h>

#include "aether/book/level_bitmap.hpp"
#include "aether/book/price_level.hpp"

using namespace aether;

TEST(PriceLevel, PushBackKeepsFifoOrderAndTotals) {
    PriceLevel level;
    Order      a, b, c;
    a.qty = 10;
    b.qty = 20;
    c.qty = 30;
    level.push_back(&a);
    level.push_back(&b);
    level.push_back(&c);
    EXPECT_EQ(level.head, &a);
    EXPECT_EQ(level.tail, &c);
    EXPECT_EQ(a.next, &b);
    EXPECT_EQ(c.prev, &b);
    EXPECT_EQ(level.total, 60);
    EXPECT_EQ(level.count, 3u);
    EXPECT_EQ(b.level, &level);
}

TEST(PriceLevel, UnlinkFromMiddleFrontAndBack) {
    PriceLevel level;
    Order      a, b, c;
    a.qty = 10;
    b.qty = 20;
    c.qty = 30;
    level.push_back(&a);
    level.push_back(&b);
    level.push_back(&c);

    level.unlink(&b); // middle
    EXPECT_EQ(a.next, &c);
    EXPECT_EQ(c.prev, &a);
    EXPECT_EQ(level.total, 40);

    level.unlink(&a); // front
    EXPECT_EQ(level.head, &c);
    EXPECT_EQ(c.prev, nullptr);

    level.unlink(&c); // last one
    EXPECT_TRUE(level.empty());
    EXPECT_EQ(level.tail, nullptr);
    EXPECT_EQ(level.total, 0);
    EXPECT_EQ(level.count, 0u);
}

TEST(LevelBitmap, FindsNextAndPreviousAcrossWords) {
    LevelBitmap bits(300);
    EXPECT_EQ(bits.next_at_or_above(0), LevelBitmap::npos);
    EXPECT_EQ(bits.prev_at_or_below(299), LevelBitmap::npos);

    bits.set(3);
    bits.set(64);
    bits.set(299);
    EXPECT_TRUE(bits.test(64));
    EXPECT_EQ(bits.next_at_or_above(0), 3u);
    EXPECT_EQ(bits.next_at_or_above(3), 3u);
    EXPECT_EQ(bits.next_at_or_above(4), 64u);
    EXPECT_EQ(bits.next_at_or_above(65), 299u);
    EXPECT_EQ(bits.next_at_or_above(300), LevelBitmap::npos);

    EXPECT_EQ(bits.prev_at_or_below(299), 299u);
    EXPECT_EQ(bits.prev_at_or_below(298), 64u);
    EXPECT_EQ(bits.prev_at_or_below(63), 3u);
    EXPECT_EQ(bits.prev_at_or_below(2), LevelBitmap::npos);

    bits.clear(64);
    EXPECT_FALSE(bits.test(64));
    EXPECT_EQ(bits.next_at_or_above(4), 299u);
}
