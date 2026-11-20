#include "alloc_counter.hpp"

#include <atomic>
#include <cstdlib>
#include <new>

namespace {
std::atomic<std::uint64_t> g_allocations{0};
}

namespace aether::bench {
std::uint64_t allocation_count() noexcept { return g_allocations.load(std::memory_order_relaxed); }
}

// Counting replacements for global new/delete. Linked only into the benchmark, never into
// the engine or the tests.
void* operator new(std::size_t size) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    if (void* p = std::malloc(size == 0 ? 1 : size)) return p;
    throw std::bad_alloc{};
}

void* operator new(std::size_t size, std::align_val_t alignment) {
    g_allocations.fetch_add(1, std::memory_order_relaxed);
    const std::size_t align   = static_cast<std::size_t>(alignment);
    const std::size_t rounded = (size + align - 1) / align * align;
    if (void* p = std::aligned_alloc(align, rounded == 0 ? align : rounded)) return p;
    throw std::bad_alloc{};
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
