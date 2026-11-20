#pragma once

#include <cstdint>

namespace aether::bench {

// Number of calls to global operator new since the program started.
std::uint64_t allocation_count() noexcept;

} // namespace aether::bench
