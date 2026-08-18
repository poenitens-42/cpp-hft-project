#pragma once

#include <cstdint>
#include <cstring>

// NASDAQ TotalView-ITCH 5.0 message layouts.
//
// Verified against the official spec (NQTVITCHSpecification_5.0.pdf,
// version 03/06/2015) — field offsets/lengths below match it exactly.
//
// All multi-byte integer fields on the wire are BIG-ENDIAN. x86 is
// little-endian, so these structs deliberately store the raw bytes
// (uint8_t arrays) rather than uint16_t/uint32_t/uint64_t — that keeps
// sizeof(Msg) an exact 1:1 match with the wire bytes, so a struct can
// be laid directly over a received/mmap'd buffer via reinterpret_cast
// with zero copy. Multi-byte fields are decoded on demand through the
// accessor methods below, not eagerly, to stay on the zero-copy /
// zero-alloc path used elsewhere in this codebase.
//
// Historical ITCH data files (e.g. NASDAQ's free daily samples) frame
// each message with a 2-byte big-endian length prefix ahead of the
// message body shown here — that prefix is NOT part of these structs.
// Confirm this framing against the actual downloaded sample before
// wiring up the parser; some distributions may differ.

namespace hft::itch {

// ------------------------------------------------------------
// Big-endian field decoders — explicit byte composition, so
// these are correct regardless of host byte order.
// ------------------------------------------------------------
inline uint16_t be16(const uint8_t* p) {
    return (static_cast<uint16_t>(p[0]) << 8) | p[1];
}

inline uint32_t be32(const uint8_t* p) {
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |  static_cast<uint32_t>(p[3]);
}

// 48-bit timestamp field (nanoseconds since midnight) — returned
// widened into a uint64_t, top 16 bits always zero.
inline uint64_t be48(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 6; ++i) v = (v << 8) | p[i];
    return v;
}

inline uint64_t be64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | p[i];
    return v;
}

// ------------------------------------------------------------
// Message type tags (offset 0 of every message)
// ------------------------------------------------------------
enum class MsgType : uint8_t {
    SystemEvent            = 'S',
    StockDirectory          = 'R',
    AddOrder                = 'A',
    AddOrderMPID             = 'F',
    OrderExecuted           = 'E',
    OrderExecutedWithPrice   = 'C',
    OrderCancel              = 'X',
    OrderDelete              = 'D',
    OrderReplace             = 'U',
    Trade                    = 'P',
    CrossTrade               = 'Q',
    BrokenTrade              = 'B',
    NOII                     = 'I',
};

// ------------------------------------------------------------
// System Event Message 'S' — 12 bytes
// ------------------------------------------------------------
struct __attribute__((packed)) SystemEventMsg {
    uint8_t msg_type;          // 'S'
    uint8_t stock_locate[2];
    uint8_t tracking_number[2];
    uint8_t timestamp[6];
    uint8_t event_code;        // 'O','S','Q','M','E','C'

    uint64_t timestamp_ns() const { return be48(timestamp); }
    static constexpr std::size_t SIZE = 12;
};
static_assert(sizeof(SystemEventMsg) == SystemEventMsg::SIZE);

// ------------------------------------------------------------
// Stock Directory 'R' — 39 bytes
// One per symbol, sent near the start of the day. Only the
// fields useful for a locate->symbol map are exposed; the rest
// are still part of the struct (for correct sizeof/offsets) but
// unaccessed.
// ------------------------------------------------------------
struct __attribute__((packed)) StockDirectoryMsg {
    uint8_t msg_type;          // 'R'
    uint8_t stock_locate[2];
    uint8_t tracking_number[2];
    uint8_t timestamp[6];
    uint8_t stock[8];           // space-padded symbol
    uint8_t market_category;
    uint8_t financial_status_indicator;
    uint8_t round_lot_size[4];
    uint8_t round_lots_only;
    uint8_t issue_classification;
    uint8_t issue_subtype[2];
    uint8_t authenticity;
    uint8_t short_sale_threshold_indicator;
    uint8_t ipo_flag;
    uint8_t luld_reference_price_tier;
    uint8_t etp_flag;
    uint8_t etp_leverage_factor[4];
    uint8_t inverse_indicator;

    uint64_t timestamp_ns() const { return be48(timestamp); }
    uint16_t locate() const { return be16(stock_locate); }

    static constexpr std::size_t SIZE = 39;
};
static_assert(sizeof(StockDirectoryMsg) == StockDirectoryMsg::SIZE);

// ------------------------------------------------------------
// Add Order — No MPID Attribution 'A' — 36 bytes
// ------------------------------------------------------------
struct __attribute__((packed)) AddOrderMsg {
    uint8_t msg_type;          // 'A'
    uint8_t stock_locate[2];
    uint8_t tracking_number[2];
    uint8_t timestamp[6];
    uint8_t order_ref_number[8];
    uint8_t buy_sell_indicator; // 'B' or 'S'
    uint8_t shares[4];
    uint8_t stock[8];           // space-padded symbol
    uint8_t price[4];           // Price(4) — 4 implied decimal places

    uint64_t timestamp_ns() const { return be48(timestamp); }
    uint64_t order_ref() const { return be64(order_ref_number); }
    bool is_buy() const { return buy_sell_indicator == 'B'; }
    uint32_t share_count() const { return be32(shares); }
    uint32_t raw_price() const { return be32(price); }        // divide by 10000.0 for decimal
    double price_decimal() const { return raw_price() / 10000.0; }

    static constexpr std::size_t SIZE = 36;
};
static_assert(sizeof(AddOrderMsg) == AddOrderMsg::SIZE);

// ------------------------------------------------------------
// Add Order — MPID Attribution 'F' — 40 bytes
// Same layout as AddOrderMsg plus a 4-byte attribution field.
// ------------------------------------------------------------
struct __attribute__((packed)) AddOrderMPIDMsg {
    uint8_t msg_type;          // 'F'
    uint8_t stock_locate[2];
    uint8_t tracking_number[2];
    uint8_t timestamp[6];
    uint8_t order_ref_number[8];
    uint8_t buy_sell_indicator;
    uint8_t shares[4];
    uint8_t stock[8];
    uint8_t price[4];
    uint8_t attribution[4];     // MPID, alpha

    uint64_t timestamp_ns() const { return be48(timestamp); }
    uint64_t order_ref() const { return be64(order_ref_number); }
    bool is_buy() const { return buy_sell_indicator == 'B'; }
    uint32_t share_count() const { return be32(shares); }
    uint32_t raw_price() const { return be32(price); }
    double price_decimal() const { return raw_price() / 10000.0; }

    static constexpr std::size_t SIZE = 40;
};
static_assert(sizeof(AddOrderMPIDMsg) == AddOrderMPIDMsg::SIZE);

// ------------------------------------------------------------
// Order Executed 'E' — 31 bytes
// ------------------------------------------------------------
struct __attribute__((packed)) OrderExecutedMsg {
    uint8_t msg_type;          // 'E'
    uint8_t stock_locate[2];
    uint8_t tracking_number[2];
    uint8_t timestamp[6];
    uint8_t order_ref_number[8];
    uint8_t executed_shares[4];
    uint8_t match_number[8];

    uint64_t timestamp_ns() const { return be48(timestamp); }
    uint64_t order_ref() const { return be64(order_ref_number); }
    uint32_t shares_executed() const { return be32(executed_shares); }
    uint64_t match_num() const { return be64(match_number); }

    static constexpr std::size_t SIZE = 31;
};
static_assert(sizeof(OrderExecutedMsg) == OrderExecutedMsg::SIZE);

// ------------------------------------------------------------
// Order Executed With Price 'C' — 36 bytes
// ------------------------------------------------------------
struct __attribute__((packed)) OrderExecutedWithPriceMsg {
    uint8_t msg_type;          // 'C'
    uint8_t stock_locate[2];
    uint8_t tracking_number[2];
    uint8_t timestamp[6];
    uint8_t order_ref_number[8];
    uint8_t executed_shares[4];
    uint8_t match_number[8];
    uint8_t printable;          // 'Y' or 'N'
    uint8_t execution_price[4];

    uint64_t timestamp_ns() const { return be48(timestamp); }
    uint64_t order_ref() const { return be64(order_ref_number); }
    uint32_t shares_executed() const { return be32(executed_shares); }
    uint64_t match_num() const { return be64(match_number); }
    bool is_printable() const { return printable == 'Y'; }
    uint32_t raw_price() const { return be32(execution_price); }
    double price_decimal() const { return raw_price() / 10000.0; }

    static constexpr std::size_t SIZE = 36;
};
static_assert(sizeof(OrderExecutedWithPriceMsg) == OrderExecutedWithPriceMsg::SIZE);

// ------------------------------------------------------------
// Order Cancel (partial) 'X' — 23 bytes
// ------------------------------------------------------------
struct __attribute__((packed)) OrderCancelMsg {
    uint8_t msg_type;          // 'X'
    uint8_t stock_locate[2];
    uint8_t tracking_number[2];
    uint8_t timestamp[6];
    uint8_t order_ref_number[8];
    uint8_t cancelled_shares[4];

    uint64_t timestamp_ns() const { return be48(timestamp); }
    uint64_t order_ref() const { return be64(order_ref_number); }
    uint32_t shares_cancelled() const { return be32(cancelled_shares); }

    static constexpr std::size_t SIZE = 23;
};
static_assert(sizeof(OrderCancelMsg) == OrderCancelMsg::SIZE);

// ------------------------------------------------------------
// Order Delete (full cancel) 'D' — 19 bytes
// ------------------------------------------------------------
struct __attribute__((packed)) OrderDeleteMsg {
    uint8_t msg_type;          // 'D'
    uint8_t stock_locate[2];
    uint8_t tracking_number[2];
    uint8_t timestamp[6];
    uint8_t order_ref_number[8];

    uint64_t timestamp_ns() const { return be48(timestamp); }
    uint64_t order_ref() const { return be64(order_ref_number); }

    static constexpr std::size_t SIZE = 19;
};
static_assert(sizeof(OrderDeleteMsg) == OrderDeleteMsg::SIZE);

// ------------------------------------------------------------
// Order Replace (cancel-replace) 'U' — 35 bytes
// Side/symbol/MPID are NOT included — carried over from the
// original Add Order per the spec.
// ------------------------------------------------------------
struct __attribute__((packed)) OrderReplaceMsg {
    uint8_t msg_type;          // 'U'
    uint8_t stock_locate[2];
    uint8_t tracking_number[2];
    uint8_t timestamp[6];
    uint8_t original_order_ref_number[8];
    uint8_t new_order_ref_number[8];
    uint8_t shares[4];
    uint8_t price[4];

    uint64_t timestamp_ns() const { return be48(timestamp); }
    uint64_t original_order_ref() const { return be64(original_order_ref_number); }
    uint64_t new_order_ref() const { return be64(new_order_ref_number); }
    uint32_t share_count() const { return be32(shares); }
    uint32_t raw_price() const { return be32(price); }
    double price_decimal() const { return raw_price() / 10000.0; }

    static constexpr std::size_t SIZE = 35;
};
static_assert(sizeof(OrderReplaceMsg) == OrderReplaceMsg::SIZE);

// ------------------------------------------------------------
// Trade Message (Non-Cross) 'P' — 44 bytes
// Non-displayable order match. Doesn't affect the book, but
// useful for time & sales / volume if you want it later.
// ------------------------------------------------------------
struct __attribute__((packed)) TradeMsg {
    uint8_t msg_type;          // 'P'
    uint8_t stock_locate[2];
    uint8_t tracking_number[2];
    uint8_t timestamp[6];
    uint8_t order_ref_number[8]; // always 0 since Dec 2010
    uint8_t buy_sell_indicator;  // always 'B' since Jul 2014
    uint8_t shares[4];
    uint8_t stock[8];
    uint8_t price[4];
    uint8_t match_number[8];

    uint64_t timestamp_ns() const { return be48(timestamp); }
    uint32_t share_count() const { return be32(shares); }
    uint32_t raw_price() const { return be32(price); }
    double price_decimal() const { return raw_price() / 10000.0; }
    uint64_t match_num() const { return be64(match_number); }

    static constexpr std::size_t SIZE = 44;
};
static_assert(sizeof(TradeMsg) == TradeMsg::SIZE);

} // namespace hft::itch
