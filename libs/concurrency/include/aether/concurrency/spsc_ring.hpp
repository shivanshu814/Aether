#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <type_traits>

namespace aether {

// Lock-free single-producer single-consumer ring buffer.
//
// Exactly one thread may call try_push and exactly one (other) thread may call try_pop.
// The producer owns tail_, the consumer owns head_. A slot is published by the producer's
// release-store of tail_ and observed by the consumer's acquire-load, which is what makes
// the slot's contents visible. head_ works the same way in the other direction.
//
// head_ and tail_ sit on separate cache lines so the two cores do not fight over one line
// (false sharing). Each side also keeps a cached copy of the other side's index and only
// re-reads the shared atomic when the ring looks full or empty.
template <class T>
    requires std::is_trivially_copyable_v<T>
class SpscRing {
public:
    // Capacity is rounded up to a power of two so wrap-around is a mask, not a modulo.
    explicit SpscRing(std::size_t capacity)
        : capacity_(std::bit_ceil(capacity < 2 ? std::size_t{2} : capacity)),
          mask_(capacity_ - 1),
          buffer_(std::make_unique<T[]>(capacity_)) {}

    bool try_push(const T& value) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail - head_cache_ == capacity_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (tail - head_cache_ == capacity_) return false; // full
        }
        buffer_[tail & mask_] = value;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        if (head == tail_cache_) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (head == tail_cache_) return false; // empty
        }
        out = buffer_[head & mask_];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    std::size_t capacity() const noexcept { return capacity_; }

private:
    static constexpr std::size_t kCacheLine = 64;

    const std::size_t          capacity_;
    const std::size_t          mask_;
    const std::unique_ptr<T[]> buffer_;

    // consumer side
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    std::size_t                                  tail_cache_ = 0;

    // producer side
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::size_t                                  head_cache_ = 0;
};

} // namespace aether
