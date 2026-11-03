#pragma once

#include <cstdint>

#include "lob/types.hpp"

namespace lob {

struct PriceLevel;

// A resting order. The prev/next pointers make it a node in its price level's
// FIFO queue (an intrusive list), so the book never allocates list nodes and
// can unlink any order in O(1) given only a pointer to it.
struct Order {
  OrderId id = 0;
  Price price = 0;
  Qty qty = 0;  // remaining quantity
  Side side = Side::Buy;
  Order* prev = nullptr;
  Order* next = nullptr;
  PriceLevel* level = nullptr;
};

// All resting orders at one price, oldest first (time priority).
struct PriceLevel {
  Price price = 0;
  Qty total_qty = 0;
  std::uint32_t count = 0;
  Order* head = nullptr;  // oldest: next to trade
  Order* tail = nullptr;  // newest

  bool empty() const noexcept { return head == nullptr; }

  void push_back(Order* o) noexcept {
    o->level = this;
    o->next = nullptr;
    o->prev = tail;
    if (tail) {
      tail->next = o;
    } else {
      head = o;
    }
    tail = o;
    total_qty += o->qty;
    ++count;
  }

  // O(1) unlink from anywhere in the queue.
  void erase(Order* o) noexcept {
    if (o->prev) {
      o->prev->next = o->next;
    } else {
      head = o->next;
    }
    if (o->next) {
      o->next->prev = o->prev;
    } else {
      tail = o->prev;
    }
    total_qty -= o->qty;
    --count;
    o->prev = o->next = nullptr;
    o->level = nullptr;
  }
};

}  // namespace lob
