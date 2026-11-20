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
    EXPECT_FALSE(ring.try_pop(out));
    for (int i = 0; i < 4; ++i) EXPECT_TRUE(ring.try_push(i));
    EXPECT_FALSE(ring.try_push(99));
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

TEST(SpscRing, BatchPushAndPopMoveWhatFits) {
    SpscRing<int> ring(8);
    const int     in[6] = {1, 2, 3, 4, 5, 6};
    EXPECT_EQ(ring.try_push_n(in, 6), 6u);
    EXPECT_EQ(ring.try_push_n(in, 6), 2u);
    int out[16] = {};
    EXPECT_EQ(ring.try_pop_n(out, 3), 3u);
    EXPECT_EQ(out[0], 1);
    EXPECT_EQ(out[2], 3);
    EXPECT_EQ(ring.try_pop_n(out, 16), 5u);
    EXPECT_EQ(out[0], 4);
    EXPECT_EQ(out[3], 1);
    EXPECT_EQ(out[4], 2);
    EXPECT_EQ(ring.try_pop_n(out, 16), 0u);
    EXPECT_EQ(ring.try_push_n(in, 0), 0u);
}

TEST(SpscRing, TwoThreadsBatchedDeliverEverythingInOrder) {
    constexpr std::uint64_t kCount = 500'000;
    SpscRing<std::uint64_t> ring(256);

    std::thread producer([&] {
        std::uint64_t next = 0;
        std::uint64_t chunk[37];
        while (next < kCount) {
            std::size_t want = 0;
            while (want < 37 && next + want < kCount) {
                chunk[want] = next + want;
                ++want;
            }
            std::size_t sent = 0;
            while (sent < want) {
                const std::size_t n = ring.try_push_n(chunk + sent, want - sent);
                if (n == 0) std::this_thread::yield();
                sent += n;
            }
            next += want;
        }
    });

    std::uint64_t expected = 0;
    std::uint64_t out[64];
    bool          in_order = true;
    while (expected < kCount) {
        const std::size_t n = ring.try_pop_n(out, 64);
        if (n == 0) std::this_thread::yield();
        for (std::size_t i = 0; i < n; ++i) in_order = in_order && out[i] == expected++;
    }
    producer.join();
    EXPECT_TRUE(in_order);
}
