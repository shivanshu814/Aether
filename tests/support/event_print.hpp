#pragma once

#include <ostream>

#include "aether/core/events.hpp"

// Lets GoogleTest print an Event as readable text instead of 48 raw bytes.
namespace aether {

inline const char* to_string(EventType t) {
    switch (t) {
    case EventType::Ack:      return "Ack";
    case EventType::Trade:    return "Trade";
    case EventType::Rested:   return "Rested";
    case EventType::Expired:  return "Expired";
    case EventType::Canceled: return "Canceled";
    case EventType::Reject:   return "Reject";
    case EventType::Modified: return "Modified";
    }
    return "?";
}

inline void PrintTo(const Event& e, std::ostream* os) {
    *os << to_string(e.type) << "{seq=" << e.seq << " order=" << e.order_id << " maker=" << e.maker_id
        << " price=" << e.price << " qty=" << e.qty << " inst=" << e.instrument
        << " side=" << (e.side == Side::Buy ? "Buy" : "Sell")
        << " reason=" << static_cast<int>(e.reason) << "}";
}

} // namespace aether
