#pragma once

#include <cstdint>

#include "aether/core/event_sink.hpp"

namespace aether {

// FNV-1a, 64-bit. Values are fed one byte at a time, low byte first, so the result does not
// depend on the machine's byte order or on struct padding.
class Fnv1a {
public:
    constexpr void mix(std::uint64_t value) noexcept {
        for (int i = 0; i < 8; ++i) {
            hash_ ^= (value >> (8 * i)) & 0xFFU;
            hash_ *= 1099511628211ULL;
        }
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
