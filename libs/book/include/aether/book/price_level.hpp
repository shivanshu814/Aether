#pragma once

#include <cstdint>

#include "aether/book/order.hpp"

namespace aether {

// All orders resting at one price: an intrusive doubly linked FIFO.
// head is the oldest order and fills first; new orders go to tail.
struct PriceLevel {
    Order*        head  = nullptr;
    Order*        tail  = nullptr;
    Qty           total = 0; // sum of remaining qty
    std::uint32_t count = 0;

    bool empty() const noexcept { return head == nullptr; }

    void push_back(Order* o) noexcept {
        o->level = this;
        o->next  = nullptr;
        o->prev  = tail;
        if (tail != nullptr) {
            tail->next = o;
        } else {
            head = o;
        }
        tail = o;
        total += o->qty;
        ++count;
    }

    // O(1) removal from anywhere in the queue.
    void unlink(Order* o) noexcept {
        if (o->prev != nullptr) {
            o->prev->next = o->next;
        } else {
            head = o->next;
        }
        if (o->next != nullptr) {
            o->next->prev = o->prev;
        } else {
            tail = o->prev;
        }
        total -= o->qty;
        --count;
        o->prev = o->next = nullptr;
        o->level          = nullptr;
    }
};

} // namespace aether
