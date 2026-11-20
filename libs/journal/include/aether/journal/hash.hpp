#pragma once

#include <cstdint>

#include "aether/core/event_sink.hpp"

namespace aether {

// FNV-1a over 64-bit words: xor the value in, multiply by the FNV prime. Classic FNV-1a feeds
// one byte per step; feeding a whole field per step is 8x fewer multiplies, and every value
// here is an integer field, so the result does not depend on byte order or struct padding.
// It is used to check that two runs produced the same events, not as a general-purpose hash.
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

// An EventSink that folds every event into one running hash, field by field.
// Two runs produced the same output if and only if hash and count match.
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

} // namespace aether
