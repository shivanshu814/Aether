#pragma once
#include "aether/core/events.hpp"

namespace aether {
    template <class S>
    concept EventSink = requires(S& sink, const Event& e){
        { sink.emit(e) } noexcept;
    };
} //namespace aether