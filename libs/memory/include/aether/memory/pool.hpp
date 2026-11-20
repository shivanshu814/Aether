#pragma once

#include <cstddef>
#include <memory>
#include <utility>

namespace aether {

// Fixed-capacity object pool. One array of slots is allocated in the constructor; after that
// create() and destroy() are a couple of pointer moves and never touch the heap.
// A free slot stores the "next free" pointer in its own bytes, so the free list costs nothing.
// Not thread-safe: only the engine thread uses it.
template <class T>
class Pool {
public:
    explicit Pool(std::size_t capacity)
        : slots_(std::make_unique<Slot[]>(capacity)), capacity_(capacity) {
        for (std::size_t i = 0; i < capacity; ++i) {
            slots_[i].next = free_head_;
            free_head_     = &slots_[i];
        }
    }

    Pool(const Pool&)            = delete;
    Pool& operator=(const Pool&) = delete;
    Pool(Pool&&) noexcept            = default;
    Pool& operator=(Pool&&) noexcept = default;

    // Returns nullptr when the pool is exhausted. It never falls back to new.
    template <class... Args>
    T* create(Args&&... args) {
        if (free_head_ == nullptr) return nullptr;
        Slot* slot = free_head_;
        free_head_ = slot->next;
        ++in_use_;
        return std::construct_at(reinterpret_cast<T*>(slot->storage), std::forward<Args>(args)...);
    }

    void destroy(T* p) noexcept {
        std::destroy_at(p);
        Slot* slot = reinterpret_cast<Slot*>(p);
        slot->next = free_head_;
        free_head_ = slot;
        --in_use_;
    }

    std::size_t in_use() const noexcept { return in_use_; }
    std::size_t capacity() const noexcept { return capacity_; }
    bool        full() const noexcept { return in_use_ == capacity_; }

private:
    union Slot {
        Slot* next;
        alignas(T) std::byte storage[sizeof(T)];
    };

    std::unique_ptr<Slot[]> slots_;
    Slot*                   free_head_ = nullptr;
    std::size_t             capacity_  = 0;
    std::size_t             in_use_    = 0;
};

} // namespace aether
