#include "order_book.hpp"
#include <cassert>
#include <iostream>

// ============================================================
// order_book_test.cpp
//
// Exercises the LOB with realistic ES futures scenarios.
// Not a unit test framework — readable assertions that print


using namespace hft;

void test_basic_insert_and_best_price() {
    std::cout << "\n--- test_basic_insert_and_best_price ---\n";
    LimitOrderBook book;

    book.add_order(1, 4500.00, 10, true);  // bid
    book.add_order(2, 4500.25, 5,  true);  // bid — better (higher)
    book.add_order(3, 4500.75, 8,  false); // ask
    book.add_order(4, 4500.50, 12, false); // ask — better (lower)

    assert(book.best_bid() == 4500.25);
    assert(book.best_ask() == 4500.50);
    assert(book.spread_ticks() == 1); // 4500.50 - 4500.25 = 0.25 = 1 tick

    std::cout << "  best_bid  = " << book.best_bid()   << " (expected 4500.25)\n";
    std::cout << "  best_ask  = " << book.best_ask()   << " (expected 4500.50)\n";
    std::cout << "  spread    = " << book.spread()     << " (expected 0.25)\n";
    std::cout << "  PASSED\n";
}

void test_price_time_priority() {
    std::cout << "\n--- test_price_time_priority ---\n";
    LimitOrderBook book;

    // Three orders at same price — FIFO queue
    book.add_order(10, 4500.00, 5,  true);
    book.add_order(11, 4500.00, 3,  true);
    book.add_order(12, 4500.00, 7,  true);

    // All three should be at the same level
    int64_t tick = Order::to_ticks(4500.00);
    int64_t idx  = hft::Side<true>::tick_to_index(tick);
    assert(book.bids.levels[idx].count == 3);
    assert(book.best_bid_qty() == 15); // 5+3+7

    std::cout << "  orders at 4500.00: " << book.bids.levels[idx].count
              << " (expected 3)\n";
    std::cout << "  total qty at best: " << book.best_bid_qty()
              << " (expected 15)\n";
    std::cout << "  PASSED\n";
}

void test_cancel_order() {
    std::cout << "\n--- test_cancel_order ---\n";
    LimitOrderBook book;

    book.add_order(20, 4500.00, 10, true);
    book.add_order(21, 4500.25, 5,  true); // best bid

    assert(book.best_bid() == 4500.25);

    // Cancel the best bid
    book.cancel_order(21, 4500.25, true);

    // Best should now fall back to 4500.00
    assert(book.best_bid() == 4500.00);
    std::cout << "  after cancel best_bid = " << book.best_bid()
              << " (expected 4500.00)\n";

    // Cancel the remaining order
    book.cancel_order(20, 4500.00, true);
    assert(book.bids.empty());
    std::cout << "  after all cancels: bids empty = "
              << (book.bids.empty() ? "true" : "false")
              << " (expected true)\n";
    std::cout << "  PASSED\n";
}

void test_modify_order() {
    std::cout << "\n--- test_modify_order ---\n";
    LimitOrderBook book;

    book.add_order(30, 4500.00, 10, true);
    assert(book.best_bid_qty() == 10);

    book.modify_order(30, 4500.00, 25, true);
    assert(book.best_bid_qty() == 25);

    std::cout << "  qty after modify = " << book.best_bid_qty()
              << " (expected 25)\n";
    std::cout << "  PASSED\n";
}

void test_circular_wraparound() {
    std::cout << "\n--- test_circular_wraparound ---\n";
    // DEPTH = 1024 ticks
    // Two prices exactly DEPTH ticks apart map to same slot
    // We must detect and evict the stale one

    LimitOrderBook book;

    // Price A at tick T
    double price_a = 4500.00;
    int64_t tick_a = Order::to_ticks(price_a); // e.g. 18000

    // Price B = price_a + DEPTH ticks
    double price_b = Order::to_price(tick_a + DEPTH); // 18000 + 1024 = 19024 ticks = 4756.00

    book.add_order(40, price_a, 10, true);
    assert(book.best_bid() == price_a);

    // Adding price_b should evict price_a's slot (same index, different tick)
    book.add_order(41, price_b, 5, true);
    assert(book.best_bid() == price_b); // price_b is higher = better bid

    std::cout << "  price_a = " << price_a << " (tick " << tick_a << ")\n";
    std::cout << "  price_b = " << price_b << " (tick " << tick_a + DEPTH << ")\n";
    std::cout << "  best_bid after both inserts = " << book.best_bid()
              << " (expected " << price_b << ")\n";
    std::cout << "  PASSED\n";
}

void demo_realistic_book() {
    std::cout << "\n--- demo_realistic_book ---\n";
    LimitOrderBook book;

    // Simulate a realistic ES futures book around 4500
    // Asks above mid
    book.add_order(100, 4500.50, 12, false);
    book.add_order(101, 4500.75, 8,  false);
    book.add_order(102, 4500.75, 4,  false); // second order at same ask level
    book.add_order(103, 4501.00, 20, false);
    book.add_order(104, 4501.25, 15, false);
    book.add_order(105, 4501.50, 6,  false);

    // Bids below mid
    book.add_order(200, 4500.25, 10, true);
    book.add_order(201, 4500.25, 5,  true);  // second order at same bid level
    book.add_order(202, 4500.00, 18, true);
    book.add_order(203, 4499.75, 25, true);
    book.add_order(204, 4499.50, 30, true);
    book.add_order(205, 4499.25, 12, true);

    book.print(5);
}

int main() {
    std::cout << "========== Order Book Tests ==========\n";

    test_basic_insert_and_best_price();
    test_price_time_priority();
    test_cancel_order();
    test_modify_order();
    test_circular_wraparound();
    demo_realistic_book();

    std::cout << "\n========== All tests passed ==========\n";
    return 0;
}
