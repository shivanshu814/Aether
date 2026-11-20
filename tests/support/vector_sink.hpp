#pragma once
#include <vector>
#include "aether/core/event_sink.hpp"
#include "support/event_print.hpp"

namespace aether::test {
    struct VectorSink {
        std::vector<Event> events;
        void emit(const Event& e) noexcept {
            events.push_back(e);
        }
        void clear() noexcept {
            events.clear();
        }
    };
    static_assert(EventSink<VectorSink>);
}
