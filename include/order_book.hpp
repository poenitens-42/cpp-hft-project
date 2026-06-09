#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <optional>


// hft::LimitOrderBook
//
// Circular array LOB for ES futures (tick = 0.25)
//


namespace hft {


// Constants — ES futures
static constexpr double   ES_TICK_SIZE  = 0.25;
static constexpr int64_t  DEPTH         = 1024;       // must be power of 2
static constexpr int64_t  DEPTH_MASK    = DEPTH - 1;  // bitmask for fast modulo
static constexpr int64_t  ORDERS_PER_LEVEL = 8;       // max orders at one price


// Order
// POD struct — no virtual, no heap, fits in one cache line (48B)
struct Order {
    int64_t  order_id  = 0;
    int64_t  price_ticks = 0;   // price in integer ticks (price / ES_TICK_SIZE)
    int32_t  quantity  = 0;
    bool     is_bid    = false;
    bool     active    = false; // false = slot is empty

    // Convert real price to ticks — call once at boundary, never inside hot path
    static int64_t to_ticks(double price) {
        // Multiply then round to nearest int — avoids truncation error
        // e.g. 4500.25 / 0.25 = 18001.0 exactly in IEEE 754
        // but 4500.50 / 0.25 could be 18001.9999... → round fixes it
        return static_cast<int64_t>(price / ES_TICK_SIZE + 0.5);
    }

    static double to_price(int64_t ticks) {
        return static_cast<double>(ticks) * ES_TICK_SIZE;
    }
};

// ------------------------------------------------------------
// PriceLevel — FIFO queue of up to ORDERS_PER_LEVEL orders
//
// Fixed-size array — zero heap allocation.
// Insertion at tail, removal by order_id (linear scan, max 8).
// For N=8 this is faster than a linked list due to cache locality.
// ------------------------------------------------------------
struct PriceLevel {
    std::array<Order, ORDERS_PER_LEVEL> orders{};
    int32_t count = 0;  // active orders at this level

    bool empty() const { return count == 0; }

    // Returns total quantity at this level
    int64_t total_qty() const {
        int64_t total = 0;
        for (int i = 0; i < ORDERS_PER_LEVEL; ++i)
            if (orders[i].active) total += orders[i].quantity;
        return total;
    }

    // Add order — returns false if level is full
    bool add(const Order& o) {
        for (int i = 0; i < ORDERS_PER_LEVEL; ++i) {
            if (!orders[i].active) {
                orders[i] = o;
                orders[i].active = true;
                ++count;
                return true;
            }
        }
        return false; // level full — in production: log and drop
    }

    // Cancel by order_id — O(ORDERS_PER_LEVEL) = O(1) amortised
    bool cancel(int64_t order_id) {
        for (int i = 0; i < ORDERS_PER_LEVEL; ++i) {
            if (orders[i].active && orders[i].order_id == order_id) {
                orders[i] = Order{}; // reset slot
                --count;
                return true;
            }
        }
        return false;
    }

    // Modify quantity of existing order
    bool modify(int64_t order_id, int32_t new_qty) {
        for (int i = 0; i < ORDERS_PER_LEVEL; ++i) {
            if (orders[i].active && orders[i].order_id == order_id) {
                orders[i].quantity = new_qty;
                return true;
            }
        }
        return false;
    }

    void clear() {
        orders.fill(Order{});
        count = 0;
    }
};

// ------------------------------------------------------------
// Side — one half of the order book (bids or asks)
//
// Template param IsBid:
//   true  → bids, best = highest price (ptr_best tracks max tick)
//   false → asks, best = lowest  price (ptr_best tracks min tick)
//
// This eliminates the ~100 line duplication in the book.
// Both sides share identical circular buffer mechanics —
// only the definition of "best" differs.
// ------------------------------------------------------------
template<bool IsBid>
struct Side {
    std::array<PriceLevel, DEPTH> levels{};

    // Index of best price level (-1 = empty book)
    // Bids: highest price = best
    // Asks: lowest  price = best
    int64_t best_idx  = -1;
    int64_t base_tick = -1; // tick value at index 0 (anchor)
	std::atomic<uint64_t> rescan_count{0};

    // ------------------------------------------------------------
    // tick_to_index — O(1) circular mapping
    //
    // key insight: index = tick & DEPTH_MASK
    // This works because DEPTH is a power of 2.
    // Two prices that differ by DEPTH map to the same slot —
    // we detect stale slots by checking stored order's price_ticks.
    // ------------------------------------------------------------
    static int64_t tick_to_index(int64_t tick) {
        // Handles negative ticks correctly (& on signed is impl-defined in C++17,
        // but on two's complement machines — which all modern HFT hardware is —
        // this is safe. C++20 mandates two's complement, so we're clean.)
        return tick & DEPTH_MASK;
    }

    bool empty() const { return best_idx == -1; }

    // Best price tick (-1 if empty)
    int64_t best_tick() const { return best_idx; }

    // Best price as double (for display only — never use in hot path)
    double best_price() const {
        return empty() ? 0.0 : Order::to_price(best_idx);
    }

    // Total quantity at best price
    int64_t best_qty() const {
        if (empty()) return 0;
        return levels[tick_to_index(best_idx)].total_qty();
    }

    // ----------------------------------------------------------
    // add_order
    // ----------------------------------------------------------
    bool add_order(const Order& o) {
        int64_t tick = o.price_ticks;
        int64_t idx  = tick_to_index(tick);

        // Stale slot detection: if the slot holds orders from a
        // different tick (price wrapped around), clear it first.
        if (!levels[idx].empty()) {
            // Check if existing orders belong to a different price
            for (auto& slot : levels[idx].orders) {
                if (slot.active && slot.price_ticks != tick) {
                    // Price collision — wrap-around invalidated this slot
                    levels[idx].clear();
                    break;
                }
            }
        }

        bool ok = levels[idx].add(o);

        // Update best price pointer
        if (ok) {
            if (best_idx == -1) {
                best_idx = tick;
            } else if constexpr (IsBid) {
                if (tick > best_idx) best_idx = tick; // higher = better for bids
            } else {
                if (tick < best_idx) best_idx = tick; // lower = better for asks
            }
        }
        return ok;
    }

    // ----------------------------------------------------------
    // cancel_order
    // ----------------------------------------------------------
    bool cancel_order(int64_t order_id, int64_t price_ticks) {
        int64_t idx = tick_to_index(price_ticks);
        bool ok = levels[idx].cancel(order_id);

        // If level now empty, rescan for new best
        // This is the only non-O(1) operation — O(DEPTH) worst case
        // In practice: best moves by 1-2 ticks, scan is short
        if (ok && levels[idx].empty() && best_idx == price_ticks) {
            rescan_best();
        }
        return ok;
    }

    // ----------------------------------------------------------
    // modify_order
    // ----------------------------------------------------------
    bool modify_order(int64_t order_id, int64_t price_ticks, int32_t new_qty) {
        int64_t idx = tick_to_index(price_ticks);
        return levels[idx].modify(order_id, new_qty);
    }

    // ----------------------------------------------------------
    // rescan_best — called only when best level becomes empty
    // Walks away from old best until it finds a non-empty level
    // ----------------------------------------------------------
    void rescan_best() {
        if (best_idx == -1) return;

        // In Side<IsBid> struct — add this as a member variable:
        ++rescan_count;

		int64_t start = best_idx;
        for (int64_t i = 1; i < DEPTH; ++i) {
            int64_t candidate;
            if constexpr (IsBid)
                candidate = start - i; // bids: walk down
            else
                candidate = start + i; // asks: walk up

            int64_t idx = tick_to_index(candidate);
            if (!levels[idx].empty()) {
                // Verify slot belongs to this tick (not a wrap-around collision)
                bool valid = false;
                for (auto& slot : levels[idx].orders) {
                    if (slot.active && slot.price_ticks == candidate) {
                        valid = true;
                        break;
                    }
                }
                if (valid) {
                    best_idx = candidate;
                    return;
                }
            }
        }
        best_idx = -1; // book is empty
    }

    void print(int levels_to_show = 5) const {
        if (empty()) {
            std::cout << (IsBid ? "  [Bids empty]\n" : "  [Asks empty]\n");
            return;
        }
        for (int i = 0; i < levels_to_show; ++i) {
            int64_t tick;
            if constexpr (IsBid)
                tick = best_idx - i;
            else
                tick = best_idx + i;

            int64_t idx = tick_to_index(tick);
            int64_t qty = levels[idx].total_qty();
            if (qty > 0) {
                std::cout << "  " << (IsBid ? "BID" : "ASK")
                          << " @ " << Order::to_price(tick)
                          << " | qty: " << qty
                          << " | orders: " << levels[idx].count << "\n";
            }
        }
    }
};

// ------------------------------------------------------------
// LimitOrderBook — combines bid and ask sides
// ------------------------------------------------------------
class LimitOrderBook {
public:
    Side<true>  bids; // IsBid = true
    Side<false> asks; // IsBid = false

    // ----------------------------------------------------------
    // add_order — primary interface
    // price is real double, converted to ticks once here
    // ----------------------------------------------------------
    bool add_order(int64_t order_id, double price, int32_t qty, bool is_bid) {
        Order o;
        o.order_id    = order_id;
        o.price_ticks = Order::to_ticks(price);
        o.quantity    = qty;
        o.is_bid      = is_bid;
        o.active      = true;

        if (is_bid) return bids.add_order(o);
        else        return asks.add_order(o);
    }

    // ----------------------------------------------------------
    // cancel_order
    // ----------------------------------------------------------
    bool cancel_order(int64_t order_id, double price, bool is_bid) {
        int64_t ticks = Order::to_ticks(price);
        if (is_bid) return bids.cancel_order(order_id, ticks);
        else        return asks.cancel_order(order_id, ticks);
    }

    // ----------------------------------------------------------
    // modify_order
    // ----------------------------------------------------------
    bool modify_order(int64_t order_id, double price, int32_t new_qty, bool is_bid) {
        int64_t ticks = Order::to_ticks(price);
        if (is_bid) return bids.modify_order(order_id, ticks, new_qty);
        else        return asks.modify_order(order_id, ticks, new_qty);
    }

    // ----------------------------------------------------------
    // Best bid/ask — O(1)
    // ----------------------------------------------------------
    double best_bid()   const { return bids.best_price(); }
    double best_ask()   const { return asks.best_price(); }
    int64_t best_bid_qty() const { return bids.best_qty(); }
    int64_t best_ask_qty() const { return asks.best_qty(); }

    // ----------------------------------------------------------
    // Spread — in ticks and in price
    // ----------------------------------------------------------
    int64_t spread_ticks() const {
        if (bids.empty() || asks.empty()) return -1;
        return asks.best_tick() - bids.best_tick();
    }

    double spread() const {
        int64_t st = spread_ticks();
        return st < 0 ? -1.0 : Order::to_price(st);
    }

    // ----------------------------------------------------------
    // print — for debugging and demo, never in hot path
    // ----------------------------------------------------------
    void print(int levels_to_show = 5) const {
        std::cout << "========== Order Book ==========\n";
        std::cout << "  Spread: " << spread() << " ("
                  << spread_ticks() << " ticks)\n";
        std::cout << "--- Asks (lowest = best) ---\n";
        asks.print(levels_to_show);
        std::cout << "--- Bids (highest = best) ---\n";
        bids.print(levels_to_show);
        std::cout << "================================\n";
    }
};

} // namespace hft
