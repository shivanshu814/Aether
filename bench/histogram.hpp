#pragma once

#include <array>
#include <bit>
#include <cstdint>

namespace aether::bench {

// Fixed-size log-scale latency histogram (HdrHistogram style). Values below 64 get one
// bucket each; above that every power of two is split into 32 sub-buckets, so the relative
// error is about 3%. record() never allocates, so it can sit inside the timed loop.
class Histogram {
public:
    void record(std::uint64_t value) noexcept {
        ++buckets_[index_of(value)];
        ++count_;
        if (value > max_) max_ = value;
    }

    std::uint64_t count() const noexcept { return count_; }
    std::uint64_t max() const noexcept { return max_; }

    // Smallest bucket value v such that at least `percentile` percent of samples are <= v.
    std::uint64_t percentile(double percentile) const noexcept {
        if (count_ == 0) return 0;
        const double  wanted = percentile / 100.0 * static_cast<double>(count_);
        std::uint64_t seen   = 0;
        for (std::size_t i = 0; i < buckets_.size(); ++i) {
            seen += buckets_[i];
            if (static_cast<double>(seen) >= wanted) return upper_bound_of(i);
        }
        return max_;
    }

private:
    static constexpr std::size_t kLinear    = 64;
    static constexpr std::size_t kSubBits   = 5;
    static constexpr std::size_t kSub       = std::size_t{1} << kSubBits;
    static constexpr std::size_t kExponents = 64 - 6;

    static std::size_t index_of(std::uint64_t v) noexcept {
        if (v < kLinear) return static_cast<std::size_t>(v);
        const std::size_t exponent = 63 - static_cast<std::size_t>(std::countl_zero(v)); // >= 6
        const std::size_t sub      = static_cast<std::size_t>(v >> (exponent - kSubBits)) & (kSub - 1);
        return kLinear + (exponent - 6) * kSub + sub;
    }

    static std::uint64_t upper_bound_of(std::size_t index) noexcept {
        if (index < kLinear) return index;
        const std::size_t exponent = (index - kLinear) / kSub + 6;
        const std::size_t sub      = (index - kLinear) % kSub;
        const std::uint64_t width  = std::uint64_t{1} << (exponent - kSubBits);
        return (std::uint64_t{1} << exponent) + (sub + 1) * width - 1;
    }

    std::array<std::uint64_t, kLinear + kExponents * kSub> buckets_{};
    std::uint64_t                                          count_ = 0;
    std::uint64_t                                          max_   = 0;
};

} // namespace aether::bench
