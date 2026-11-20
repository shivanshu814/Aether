#pragma once

#include <atomic>
#include <bit>
#include <cstddef>
#include <memory>
#include <thread>
#include <type_traits>

namespace aether {

// Tells the CPU this is a spin-wait loop: cheaper than a syscall, kinder than a hot spin.
inline void cpu_relax() noexcept {
#if defined(__aarch64__) || defined(__arm__)
    __asm__ __volatile__("yield");
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#endif
}

// Spin briefly, then start giving the core away. Keeps latency low when the other side is
// about to act, without starving it when there are fewer cores than threads.
class Backoff {
public:
    void pause() noexcept {
        if (spins_ < kSpinLimit) {
            ++spins_;
            cpu_relax();
        } else {
            std::this_thread::yield();
        }
    }
    void reset() noexcept { spins_ = 0; }

private:
    static constexpr unsigned kSpinLimit = 256;
    unsigned                  spins_     = 0;
};

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

    // Batch versions: one atomic store publishes the whole batch, so the other core sees one
    // cache-line transfer per batch instead of one per item. Both return how many items moved.
    std::size_t try_push_n(const T* items, std::size_t count) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        std::size_t       free = capacity_ - (tail - head_cache_);
        if (free < count) {
            head_cache_ = head_.load(std::memory_order_acquire);
            free        = capacity_ - (tail - head_cache_);
        }
        const std::size_t n = count < free ? count : free;
        for (std::size_t i = 0; i < n; ++i) buffer_[(tail + i) & mask_] = items[i];
        if (n != 0) tail_.store(tail + n, std::memory_order_release);
        return n;
    }

    std::size_t try_pop_n(T* out, std::size_t max_count) noexcept {
        const std::size_t head      = head_.load(std::memory_order_relaxed);
        std::size_t       available = tail_cache_ - head;
        if (available == 0) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            available   = tail_cache_ - head;
        }
        const std::size_t n = max_count < available ? max_count : available;
        for (std::size_t i = 0; i < n; ++i) out[i] = buffer_[(head + i) & mask_];
        if (n != 0) head_.store(head + n, std::memory_order_release);
        return n;
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
