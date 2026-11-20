#pragma once
#include <cstdint>

namespace aether {
    using Price   = std::int64_t;
    using Qty     = std::int64_t;
    using OrderId = std::uint64_t;
    using InstrumentId = std::uint32_t;
    using SeqNo = std::uint64_t;
    using Timestamp = std::uint64_t;

    enum class Side : std::uint8_t {
        Buy,
        Sell,
    };

    enum class OrderType : std::uint8_t {
        Limit,
        Market,
        IOC,
    };

    constexpr Side opposite(Side s)noexcept {
        return s == Side::Buy ? Side::Sell : Side::Buy;
    };
}
