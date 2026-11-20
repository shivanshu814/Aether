#include <gtest/gtest.h>

#include <cstdint>
#include <thread>

#include "aether/concurrency/spsc_ring.hpp"

using aether::SpscRing;

TEST(SpscRing, CapacityRoundsUpToPowerOfTwo) {
    EXPECT_EQ(SpscRing<int>(5).capacity(), 8u);
    EXPECT_EQ(SpscRing<int>(8).capacity(), 8u);
    EXPECT_EQ(SpscRing<int>(0).capacity(), 2u);
}

TEST(SpscRing, FifoOrderAndFullEmptyOnOneThread) {
    SpscRing<int> ring(4);
    int           out = 0;
    EXPECT_FALSE(ring.try_pop(out)); // empty
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(ring.try_push(i));
    EXPECT_FALSE(ring.try_push(99)); // full
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(ring.try_pop(out));
        EXPECT_EQ(out, i);
    }
    EXPECT_FALSE(ring.try_pop(out));
}

TEST(SpscRing, WrapsAroundManyTimes) {
    SpscRing<int> ring(4);
    int           out = 0;
    for (int i = 0; i < 1'000; ++i) {
        ASSERT_TRUE(ring.try_push(i));
        ASSERT_TRUE(ring.try_pop(out));
        ASSERT_EQ(out, i);
    }
}

// One producer thread, one consumer thread. Every value must arrive exactly once, in order.
// Run under the tsan preset this also checks the memory ordering for data races.
TEST(SpscRing, TwoThreadsDeliverEverythingInOrder) {
    constexpr std::uint64_t kCount = 500'000;
    SpscRing<std::uint64_t> ring(1024);

    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kCount; ++i) {
            while (!ring.try_push(i)) std::this_thread::yield();
        }
    });

    std::uint64_t expected = 0;
    std::uint64_t value    = 0;
    bool          in_order = true;
    while (expected < kCount) {
        if (ring.try_pop(value)) {
            in_order = in_order && value == expected;
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    producer.join();
    EXPECT_TRUE(in_order);
    EXPECT_FALSE(ring.try_pop(value));
}
