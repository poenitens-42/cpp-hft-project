#pragma once

#include <cstdint>
#include <cstring>

// Wire protocol — shared between bench_client and hft_app
//
// Both structs are exactly one cache line (64 bytes) or half (32 bytes).
// Fixed size = no framing overhead, no dynamic alloc, no strlen.
//
// IMPORTANT: both sides must be compiled with the same struct layout.
// We use __attribute__((packed)) to eliminate any padding surprises,
// but since all fields are naturally aligned this makes no difference

namespace hft::wire {
/* Msg - Type to distinguish from ADD and CANCEL ( needed otherwise its just monotonically adding which 
 * will saturate the price-levels */ 

	enum class MsgType : uint8_t {
		 ADD = 0,
		 CANCEL = 1,
	};

// ------------------------------------------------------------
// OrderMsg — client → server (64 bytes = 1 cache line)
// ------------------------------------------------------------
struct __attribute__((packed)) OrderMsg {
    uint64_t order_id;       // unique per message
    double   price;          // real price (e.g. 4500.25)
    int32_t  quantity;       // number of contracts
    uint8_t  is_bid;         // 1 = bid , 0 = ASK 
	uint8_t type ;           // ADD or CANCEL 
    uint8_t  pad[42];        // padding to 64 bytes

    static constexpr std::size_t SIZE = 64;
};
static_assert(sizeof(OrderMsg) == 64, "OrderMsg must be exactly 64 bytes");

// ------------------------------------------------------------
// AckMsg — server → client (32 bytes)
//
// Contains server-side RDTSC timestamps bracketing the LOB call.
// Client uses these to isolate processing latency from network RTT.
//
//   processing_ns = (t2 - t1) / tsc_ghz
//   network_ns    = rtt_ns - processing_ns
// ------------------------------------------------------------
struct __attribute__((packed)) AckMsg {
    uint64_t order_id;       // echoed back for matching
    uint64_t t1;             // RDTSC just BEFORE LOB add_order()
    uint64_t t2;             // RDTSC just AFTER  LOB add_order()
    uint8_t  accepted;       // 1 = LOB accepted, 0 = rejected (level full)
    uint8_t  pad[7];         // padding to 32 bytes

    static constexpr std::size_t SIZE = 32;
};
static_assert(sizeof(AckMsg) == 32, "AckMsg must be exactly 32 bytes");

} // namespace hft::wire
