#pragma once
#include <type_traits>
#include "aether/core/types.hpp"

namespace aether {
    enum class CommandType : std::uint8_t {
        New,
        Cancel,
        Modify,
    };

    struct Command {
        SeqNo seq = 0;
        Timestamp ts_ns = 0;
        OrderId order_id = 0;
        Price price = 0;
        Qty qty = 0;
        InstrumentId instrument = 0;
        CommandType type = CommandType::New;
        Side side = Side::Buy;
        OrderType order_type = OrderType::Limit;
        friend bool operator==(const Command&, const Command&) = default;
    };

    enum class EventType : std::uint8_t {
        Ack,
        Trade,
        Rested,
        Expired,
        Canceled,
        Reject,
        Modified,  // qty reduced in place, priority kept
    };

    enum class RejectReason : std::uint8_t {
        None,
        InvalidQty,
        InvalidPrice,
        UnknownOrder,
        DuplicateOrder,
        UnknownInstrument,
        BookFull,
        InvalidOrderId,
    };

    struct Event {
        SeqNo seq = 0;
        OrderId order_id = 0;
        OrderId maker_id = 0;
        Price price = 0;
        Qty qty = 0;
        InstrumentId instrument = 0;
        EventType type = EventType::Ack;
        Side side = Side::Buy;
        RejectReason reason = RejectReason::None;
        friend bool operator==(const Event&, const Event&) = default;
    };

    static_assert(std::is_trivially_copyable_v<Command>);
    static_assert(std::is_trivially_copyable_v<Event>);
    static_assert(sizeof(Command) == 48);
    static_assert(sizeof(Event) == 48);
} // namespace aether
