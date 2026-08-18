#pragma once

#include "protocol/itch_messages.hpp"
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <functional>

// Streaming reader for NASDAQ ITCH 5.0 files as distributed by NASDAQ's
// free historical samples (emi.nasdaq.com) — 2-byte big-endian length
// prefix, then the message body (no other framing).
//
// Verified against real production bytes (S030220-v50-bx.txt, BX
// exchange, 2020-03-02): first message decodes as length=12, type='S',
// event_code='O' (Start of Messages); second decodes as length=39,
// type='R' (Stock Directory) with a valid space-padded symbol. See
// project notes for the full byte-level walkthrough.
//
// Design: mmap the whole file (zero-copy read), walk it once using the
// self-describing length prefix to skip message types we don't have
// structs for — no per-type length table needed. Each recognized
// message is memcpy'd into a stack-local struct (not reinterpret_cast
// over the mmap'd bytes) to stay unconditionally well-defined and
// bounds-safe; see itch_messages.hpp for why memcpy over cast here.

namespace hft::itch {

// One callback per message type this parser knows how to decode.
// All default to no-ops — set only the ones you need.
struct ItchCallbacks {
    std::function<void(const SystemEventMsg&)> on_system_event;
    std::function<void(const StockDirectoryMsg&)> on_stock_directory;
    std::function<void(const AddOrderMsg&)> on_add_order;
    std::function<void(const AddOrderMPIDMsg&)> on_add_order_mpid;
    std::function<void(const OrderExecutedMsg&)> on_order_executed;
    std::function<void(const OrderExecutedWithPriceMsg&)> on_order_executed_with_price;
    std::function<void(const OrderCancelMsg&)> on_order_cancel;
    std::function<void(const OrderDeleteMsg&)> on_order_delete;
    std::function<void(const OrderReplaceMsg&)> on_order_replace;
    std::function<void(const TradeMsg&)> on_trade;
};

// Summary stats gathered during a parse pass — useful for validating
// framing integrity across an entire file before trusting it for replay.
struct ParseStats {
    uint64_t total_messages = 0;
    uint64_t total_bytes_consumed = 0;
    std::unordered_map<char, uint64_t> counts_by_type;
    uint64_t truncated_at_offset = 0;   // 0 if the file ended cleanly
    bool ended_cleanly = false;
};

// Parses `data` (length `size`) as a stream of length-prefixed ITCH
// messages, invoking `cb` for every recognized type and tallying
// per-type counts for every type (recognized or not — the length
// prefix lets us skip unknown types safely). Returns stats so the
// caller can verify the whole buffer was consumed without truncation.
inline ParseStats parse_itch_buffer(const uint8_t* data, size_t size, const ItchCallbacks& cb) {
    ParseStats stats;
    size_t pos = 0;

    while (pos + 2 <= size) {
        uint16_t msg_len = be16(data + pos);
        size_t msg_start = pos + 2;

        if (msg_len == 0 || msg_start + msg_len > size) {
            // Either a corrupt/zero length prefix, or the last message
            // is cut off (partial write, or we're mid-file on a chunk
            // boundary if this were ever called on a stream slice).
            stats.truncated_at_offset = pos;
            return stats;
        }

        char msg_type = static_cast<char>(data[msg_start]);
        stats.counts_by_type[msg_type]++;
        stats.total_messages++;

        const uint8_t* body = data + msg_start;

        switch (msg_type) {
            case 'S':
                if (msg_len == SystemEventMsg::SIZE && cb.on_system_event) {
                    SystemEventMsg m;
                    std::memcpy(&m, body, sizeof(m));
                    cb.on_system_event(m);
                }
                break;
            case 'R':
                if (msg_len == StockDirectoryMsg::SIZE && cb.on_stock_directory) {
                    StockDirectoryMsg m;
                    std::memcpy(&m, body, sizeof(m));
                    cb.on_stock_directory(m);
                }
                break;
            case 'A':
                if (msg_len == AddOrderMsg::SIZE && cb.on_add_order) {
                    AddOrderMsg m;
                    std::memcpy(&m, body, sizeof(m));
                    cb.on_add_order(m);
                }
                break;
            case 'F':
                if (msg_len == AddOrderMPIDMsg::SIZE && cb.on_add_order_mpid) {
                    AddOrderMPIDMsg m;
                    std::memcpy(&m, body, sizeof(m));
                    cb.on_add_order_mpid(m);
                }
                break;
            case 'E':
                if (msg_len == OrderExecutedMsg::SIZE && cb.on_order_executed) {
                    OrderExecutedMsg m;
                    std::memcpy(&m, body, sizeof(m));
                    cb.on_order_executed(m);
                }
                break;
            case 'C':
                if (msg_len == OrderExecutedWithPriceMsg::SIZE && cb.on_order_executed_with_price) {
                    OrderExecutedWithPriceMsg m;
                    std::memcpy(&m, body, sizeof(m));
                    cb.on_order_executed_with_price(m);
                }
                break;
            case 'X':
                if (msg_len == OrderCancelMsg::SIZE && cb.on_order_cancel) {
                    OrderCancelMsg m;
                    std::memcpy(&m, body, sizeof(m));
                    cb.on_order_cancel(m);
                }
                break;
            case 'D':
                if (msg_len == OrderDeleteMsg::SIZE && cb.on_order_delete) {
                    OrderDeleteMsg m;
                    std::memcpy(&m, body, sizeof(m));
                    cb.on_order_delete(m);
                }
                break;
            case 'U':
                if (msg_len == OrderReplaceMsg::SIZE && cb.on_order_replace) {
                    OrderReplaceMsg m;
                    std::memcpy(&m, body, sizeof(m));
                    cb.on_order_replace(m);
                }
                break;
            case 'P':
                if (msg_len == TradeMsg::SIZE && cb.on_trade) {
                    TradeMsg m;
                    std::memcpy(&m, body, sizeof(m));
                    cb.on_trade(m);
                }
                break;
            default:
                // Any other valid ITCH 5.0 type (H, Y, L, V, W, K, Q, B, I, N, ...)
                // — skip via the self-describing length, no struct needed.
                break;
        }

        pos = msg_start + msg_len;
        stats.total_bytes_consumed = pos;
    }

    stats.ended_cleanly = (pos == size);
    return stats;
}

} // namespace hft::itch
