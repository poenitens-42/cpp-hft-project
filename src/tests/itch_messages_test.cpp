#include "protocol/itch_messages.hpp"
#include <cassert>
#include <iostream>
#include <cstring>

// ============================================================
// itch_messages_test.cpp
//
// Builds synthetic wire-format bytes for each ITCH message type
// and verifies the accessor methods decode them correctly.
// Not a unit test framework — readable assertions that print.
// ============================================================

using namespace hft::itch;

// Helper: write a big-endian N-byte value into dst.
template <typename T>
void write_be(uint8_t* dst, T value, int n_bytes) {
    for (int i = 0; i < n_bytes; ++i) {
        dst[n_bytes - 1 - i] = static_cast<uint8_t>((value >> (8 * i)) & 0xFF);
    }
}

void test_be_decoders() {
    std::cout << "\n--- test_be_decoders ---\n";

    uint8_t b16[2] = {0x12, 0x34};
    assert(be16(b16) == 0x1234);

    uint8_t b32[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    assert(be32(b32) == 0xDEADBEEF);

    // 48-bit: max value should NOT bleed into bits 48-63
    uint8_t b48_max[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    assert(be48(b48_max) == 0x0000FFFFFFFFFFFFULL);

    uint8_t b48[6] = {0x00, 0x00, 0x01, 0x02, 0x03, 0x04};
    uint64_t expected48 = (uint64_t(0x00) << 40) | (uint64_t(0x00) << 32) |
                           (uint64_t(0x01) << 24) | (uint64_t(0x02) << 16) |
                           (uint64_t(0x03) << 8)  |  uint64_t(0x04);
    assert(be48(b48) == expected48);

    uint8_t b64[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};
    assert(be64(b64) == 0x0102030405060708ULL);

    std::cout << "  be16/be32/be48/be64 all correct\n";
    std::cout << "  be48 does not bleed into top 16 bits: PASSED\n";
    std::cout << "  PASSED\n";
}

void test_add_order() {
    std::cout << "\n--- test_add_order (type 'A') ---\n";

    AddOrderMsg msg{};
    msg.msg_type = 'A';
    write_be(msg.stock_locate, uint16_t(7), 2);
    write_be(msg.tracking_number, uint16_t(1), 2);
    write_be(msg.timestamp, uint64_t(34200123456789ULL), 6); // ~9:30am ns since midnight
    write_be(msg.order_ref_number, uint64_t(9876543210ULL), 8);
    msg.buy_sell_indicator = 'B';
    write_be(msg.shares, uint32_t(500), 4);
    std::memcpy(msg.stock, "AAPL    ", 8); // space-padded
    write_be(msg.price, uint32_t(1500000), 4); // 150.0000

    assert(msg.order_ref() == 9876543210ULL);
    assert(msg.is_buy() == true);
    assert(msg.share_count() == 500);
    assert(msg.raw_price() == 1500000);
    assert(msg.price_decimal() == 150.0);
    assert(msg.timestamp_ns() == 34200123456789ULL);
    assert(be16(msg.stock_locate) == 7);
    assert(std::memcmp(msg.stock, "AAPL    ", 8) == 0);

    // sell side
    msg.buy_sell_indicator = 'S';
    assert(msg.is_buy() == false);

    std::cout << "  order_ref=" << msg.order_ref() << " shares=" << msg.share_count()
              << " price=" << msg.price_decimal() << " symbol=AAPL\n";
    std::cout << "  PASSED\n";
}

void test_add_order_mpid() {
    std::cout << "\n--- test_add_order_mpid (type 'F') ---\n";

    AddOrderMPIDMsg msg{};
    msg.msg_type = 'F';
    write_be(msg.order_ref_number, uint64_t(42), 8);
    msg.buy_sell_indicator = 'B';
    write_be(msg.shares, uint32_t(100), 4);
    write_be(msg.price, uint32_t(2500000), 4); // 250.0000
    std::memcpy(msg.attribution, "NSDQ", 4);

    assert(msg.order_ref() == 42);
    assert(msg.price_decimal() == 250.0);
    assert(std::memcmp(msg.attribution, "NSDQ", 4) == 0);
    assert(sizeof(msg) == 40);

    std::cout << "  attribution=NSDQ price=" << msg.price_decimal() << "\n";
    std::cout << "  PASSED\n";
}

void test_order_executed() {
    std::cout << "\n--- test_order_executed (type 'E') ---\n";

    OrderExecutedMsg msg{};
    msg.msg_type = 'E';
    write_be(msg.order_ref_number, uint64_t(9876543210ULL), 8);
    write_be(msg.executed_shares, uint32_t(200), 4);
    write_be(msg.match_number, uint64_t(555000111ULL), 8);

    assert(msg.order_ref() == 9876543210ULL);
    assert(msg.shares_executed() == 200);
    assert(msg.match_num() == 555000111ULL);

    std::cout << "  executed 200 shares, match#=" << msg.match_num() << "\n";
    std::cout << "  PASSED\n";
}

void test_order_executed_with_price() {
    std::cout << "\n--- test_order_executed_with_price (type 'C') ---\n";

    OrderExecutedWithPriceMsg msg{};
    msg.msg_type = 'C';
    write_be(msg.order_ref_number, uint64_t(1), 8);
    write_be(msg.executed_shares, uint32_t(50), 4);
    write_be(msg.match_number, uint64_t(2), 8);
    msg.printable = 'Y';
    write_be(msg.execution_price, uint32_t(999900), 4); // 99.9900

    assert(msg.is_printable() == true);
    assert(msg.price_decimal() == 99.99);

    msg.printable = 'N';
    assert(msg.is_printable() == false);

    std::cout << "  execution_price=" << msg.price_decimal() << " printable toggled correctly\n";
    std::cout << "  PASSED\n";
}

void test_order_cancel_and_delete() {
    std::cout << "\n--- test_order_cancel_and_delete (types 'X', 'D') ---\n";

    OrderCancelMsg cancel{};
    cancel.msg_type = 'X';
    write_be(cancel.order_ref_number, uint64_t(77), 8);
    write_be(cancel.cancelled_shares, uint32_t(30), 4);
    assert(cancel.order_ref() == 77);
    assert(cancel.shares_cancelled() == 30);

    OrderDeleteMsg del{};
    del.msg_type = 'D';
    write_be(del.order_ref_number, uint64_t(88), 8);
    assert(del.order_ref() == 88);
    assert(sizeof(del) == 19);

    std::cout << "  cancel: order_ref=77 cancelled=30 shares\n";
    std::cout << "  delete: order_ref=88\n";
    std::cout << "  PASSED\n";
}

void test_order_replace() {
    std::cout << "\n--- test_order_replace (type 'U') ---\n";

    OrderReplaceMsg msg{};
    msg.msg_type = 'U';
    write_be(msg.original_order_ref_number, uint64_t(100), 8);
    write_be(msg.new_order_ref_number, uint64_t(101), 8);
    write_be(msg.shares, uint32_t(75), 4);
    write_be(msg.price, uint32_t(1234500), 4); // 123.4500

    assert(msg.original_order_ref() == 100);
    assert(msg.new_order_ref() == 101);
    assert(msg.share_count() == 75);
    assert(msg.price_decimal() == 123.45);

    std::cout << "  replaced order 100 -> 101, new_shares=75 new_price=" << msg.price_decimal() << "\n";
    std::cout << "  PASSED\n";
}

void test_trade_message() {
    std::cout << "\n--- test_trade_message (type 'P') ---\n";

    TradeMsg msg{};
    msg.msg_type = 'P';
    msg.buy_sell_indicator = 'B';
    write_be(msg.shares, uint32_t(1000), 4);
    std::memcpy(msg.stock, "MSFT    ", 8);
    write_be(msg.price, uint32_t(4200000), 4); // 420.0000
    write_be(msg.match_number, uint64_t(321), 8);

    assert(msg.share_count() == 1000);
    assert(msg.price_decimal() == 420.0);
    assert(msg.match_num() == 321);
    assert(sizeof(msg) == 44);

    std::cout << "  trade: 1000 shares @ " << msg.price_decimal() << " match#=" << msg.match_num() << "\n";
    std::cout << "  PASSED\n";
}

void test_system_event() {
    std::cout << "\n--- test_system_event (type 'S') ---\n";

    SystemEventMsg msg{};
    msg.msg_type = 'S';
    write_be(msg.timestamp, uint64_t(9000000000ULL), 6);
    msg.event_code = 'Q'; // start of market hours

    assert(msg.timestamp_ns() == 9000000000ULL);
    assert(msg.event_code == 'Q');
    assert(sizeof(msg) == 12);

    std::cout << "  event_code=Q (start of market hours) timestamp_ns=" << msg.timestamp_ns() << "\n";
    std::cout << "  PASSED\n";
}

void test_all_struct_sizes() {
    std::cout << "\n--- test_all_struct_sizes ---\n";
    // Redundant with the static_asserts in the header, but makes the
    // full set visible in one place and fails loudly at runtime too
    // if someone weakens a static_assert during a refactor.
    assert(sizeof(SystemEventMsg) == 12);
    assert(sizeof(AddOrderMsg) == 36);
    assert(sizeof(AddOrderMPIDMsg) == 40);
    assert(sizeof(OrderExecutedMsg) == 31);
    assert(sizeof(OrderExecutedWithPriceMsg) == 36);
    assert(sizeof(OrderCancelMsg) == 23);
    assert(sizeof(OrderDeleteMsg) == 19);
    assert(sizeof(OrderReplaceMsg) == 35);
    assert(sizeof(TradeMsg) == 44);
    std::cout << "  all 9 struct sizes match the NASDAQ ITCH 5.0 spec exactly\n";
    std::cout << "  PASSED\n";
}

int main() {
    test_be_decoders();
    test_add_order();
    test_add_order_mpid();
    test_order_executed();
    test_order_executed_with_price();
    test_order_cancel_and_delete();
    test_order_replace();
    test_trade_message();
    test_system_event();
    test_all_struct_sizes();

    std::cout << "\nAll ITCH message tests passed.\n";
    return 0;
}
