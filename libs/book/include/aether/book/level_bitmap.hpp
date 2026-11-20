#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace aether {

// One bit per price level: set when the level has orders. When the best level empties,
// the next non-empty one is found by scanning 64 levels per word instead of one at a time.
class LevelBitmap {
public:
    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

    explicit LevelBitmap(std::size_t bits) : words_((bits + 63) / 64, 0), bits_(bits) {}

    void set(std::size_t i) noexcept { words_[i >> 6] |= std::uint64_t{1} << (i & 63); }
    void clear(std::size_t i) noexcept { words_[i >> 6] &= ~(std::uint64_t{1} << (i & 63)); }
    bool test(std::size_t i) const noexcept { return (words_[i >> 6] >> (i & 63)) & 1U; }

    // Lowest set index >= from, or npos.
    std::size_t next_at_or_above(std::size_t from) const noexcept {
        if (from >= bits_) return npos;
        std::size_t   w    = from >> 6;
        std::uint64_t word = words_[w] & (~std::uint64_t{0} << (from & 63));
        while (true) {
            if (word != 0) return (w << 6) + static_cast<std::size_t>(std::countr_zero(word));
            if (++w == words_.size()) return npos;
            word = words_[w];
        }
    }

    // Highest set index <= from, or npos.
    std::size_t prev_at_or_below(std::size_t from) const noexcept {
        if (bits_ == 0) return npos;
        if (from >= bits_) from = bits_ - 1;
        std::size_t   w    = from >> 6;
        std::uint64_t word = words_[w] & (~std::uint64_t{0} >> (63 - (from & 63)));
        while (true) {
            if (word != 0) return (w << 6) + 63 - static_cast<std::size_t>(std::countl_zero(word));
            if (w == 0) return npos;
            word = words_[--w];
        }
    }

private:
    std::vector<std::uint64_t> words_;
    std::size_t                bits_;
};

} // namespace aether
