#include <gtest/gtest.h>

#include <set>

#include "aether/memory/pool.hpp"

using aether::Pool;

namespace {

struct Tracked {
    static inline int alive = 0;
    int               value;
    explicit Tracked(int v) : value(v) { ++alive; }
    ~Tracked() { --alive; }
};

TEST(Pool, CreatesUpToCapacityThenReturnsNull) {
    Pool<int>      pool(3);
    std::set<int*> seen;
    for (int i = 0; i < 3; ++i) {
        int* p = pool.create(i);
        ASSERT_NE(p, nullptr);
        EXPECT_EQ(*p, i);
        seen.insert(p);
    }
    EXPECT_EQ(seen.size(), 3u); // three distinct slots
    EXPECT_TRUE(pool.full());
    EXPECT_EQ(pool.create(9), nullptr); // exhausted: no fallback to new
    EXPECT_EQ(pool.in_use(), 3u);
}

TEST(Pool, DestroyedSlotIsReusedFirst) {
    Pool<int> pool(4);
    int*      a = pool.create(1);
    int*      b = pool.create(2);
    pool.destroy(a);
    EXPECT_EQ(pool.in_use(), 1u);
    int* c = pool.create(3);
    EXPECT_EQ(c, a); // LIFO free list: the slot that is still warm in cache
    EXPECT_EQ(*b, 2);
    EXPECT_EQ(*c, 3);
}

TEST(Pool, RunsConstructorsAndDestructors) {
    Pool<Tracked> pool(2);
    Tracked*      t = pool.create(7);
    EXPECT_EQ(t->value, 7);
    EXPECT_EQ(Tracked::alive, 1);
    pool.destroy(t);
    EXPECT_EQ(Tracked::alive, 0);
}

TEST(Pool, SurvivesManyCreateDestroyCycles) {
    Pool<long> pool(8);
    for (long round = 0; round < 10'000; ++round) {
        long* p = pool.create(round);
        ASSERT_NE(p, nullptr);
        ASSERT_EQ(*p, round);
        pool.destroy(p);
    }
    EXPECT_EQ(pool.in_use(), 0u);
}

} // namespace
