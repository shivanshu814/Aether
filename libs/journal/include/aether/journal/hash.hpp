#pragma once

#include <cstdint>

#include "aether/core/event_sink.hpp"

namespace aether {
// FNV-1a over 64-bit fields, never over raw struct bytes, so padding and byte order cannot
// change the hash. Used to compare runs, not as a general-purpose hash.
class Fnv1a {
public:
    constexpr void mix(std::uint64_t value) noexcept {
        hash_ ^= value;
        hash_ *= 1099511628211ULL;
    }
    constexpr std::uint64_t value() const noexcept { return hash_; }

private:
    std::uint64_t hash_ = 14695981039346656037ULL;
};

struct HashSink {
    Fnv1a         hasher;
    std::uint64_t count = 0;

    void emit(const Event& e) noexcept {
        hasher.mix(e.seq);
        hasher.mix(e.order_id);
        hasher.mix(e.maker_id);
        hasher.mix(static_cast<std::uint64_t>(e.price));
        hasher.mix(static_cast<std::uint64_t>(e.qty));
        hasher.mix(e.instrument);
        hasher.mix(static_cast<std::uint64_t>(e.type));
        hasher.mix(static_cast<std::uint64_t>(e.side));
        hasher.mix(static_cast<std::uint64_t>(e.reason));
        ++count;
    }

    std::uint64_t hash() const noexcept { return hasher.value(); }
};

static_assert(EventSink<HashSink>);
}
