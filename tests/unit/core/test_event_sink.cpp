#include <gtest/gtest.h>
#include "aether/core/event_sink.hpp"
#include "support/vector_sink.hpp"

using namespace aether;
namespace {
    struct NoEmit {};
    struct ThrowingEmit { void emit(const Event&) {} };
}

TEST(EventSink, ConceptAcceptsAndRejects) {
    static_assert(EventSink<test::VectorSink>);
    static_assert(!EventSink<NoEmit>);
    static_assert(!EventSink<ThrowingEmit>);
}

TEST(EventSink, VectorSinkCollectsInOrder) {
    test::VectorSink sink;
    Event a{};
    a.order_id = 1;
    Event b{};
    b.order_id = 2;
    sink.emit(a);
    sink.emit(b);

    ASSERT_EQ(sink.events.size(), 2u);
    ASSERT_EQ(sink.events[0], a);
    ASSERT_EQ(sink.events[1], b);
}

TEST(Events, EqualityCompareFields){
    Event a{};
    a.qty = 10;
    Event b{};
    b.qty = 10;
    Event c{};
    c.qty = 11;
    EXPECT_EQ(a,b);
    EXPECT_NE(a,c);
}
