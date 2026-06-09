#pragma once

#include <asio.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>

#include "order_book.hpp"
#include "messages.hpp"

using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::use_awaitable;
using asio::ip::tcp;
namespace this_coro = asio::this_coro;
using namespace asio::experimental::awaitable_operators;

// ------------------------------------------------------------
// RDTSC helpers — server side
//
// Same serialisation discipline as bench_client:
//   lfence + rdtsc  for START (prevents speculative reads before)
//   rdtscp          for END   (implicitly serialises after store)
// ------------------------------------------------------------
inline uint64_t tsc_start() {
    uint32_t lo, hi;
    __asm__ volatile (
        "lfence\n\t"
        "rdtsc\n\t"
        : "=a"(lo), "=d"(hi) :: "memory"
    );
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline uint64_t tsc_end() {
    uint32_t lo, hi, aux;
    __asm__ volatile (
        "rdtscp\n\t"
        : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory"
    );
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

class HFTServer {
public:
    HFTServer(asio::io_context& ctx, uint16_t port)
        : ctx_(ctx),
          acceptor_(ctx, tcp::endpoint(tcp::v4(), port))
    {}

    awaitable<void> start() {
        try {
            for (;;) {
                tcp::socket client = co_await acceptor_.async_accept(use_awaitable);
                client.set_option(tcp::no_delay(true));
                co_spawn(ctx_, handle_client(std::move(client)), detached);
            }
        } catch (const std::exception& e) {
            std::cerr << "[Server] Accept error: " << e.what() << "\n";
        }
    }

private:
    // ----------------------------------------------------------
    // handle_client
    //
    // Each connection gets its own LimitOrderBook instance.
    // In production: shared book with lock-free SPSC per session.
    // For benchmarking: per-connection book avoids lock contention
    // and gives clean isolated measurements.
    // ----------------------------------------------------------
    awaitable<void> handle_client(tcp::socket client) {
        hft::LimitOrderBook book;

        // No welcome message — removes one extra read_some() on client side
        // and eliminates a spurious RTT from the first measurement.

        std::array<char, hft::wire::OrderMsg::SIZE> recv_buf{};
        hft::wire::AckMsg ack{};

        try {
            for (;;) {
                // --- Receive exactly one OrderMsg ---
                std::size_t received = 0;
                while (received < hft::wire::OrderMsg::SIZE) {
                    std::size_t n = co_await client.async_read_some(
                        asio::buffer(recv_buf.data() + received,
                                     hft::wire::OrderMsg::SIZE - received),
                        use_awaitable);
                    received += n;
                }

                // --- Parse ---
                hft::wire::OrderMsg msg{};
                std::memcpy(&msg, recv_buf.data(), sizeof(msg));

                // --- RDTSC t1: just before LOB call ---
                uint64_t t1 = tsc_start();

                // --- Hot path: order book operation ---
                bool accepted = book.add_order(
                    static_cast<int64_t>(msg.order_id),
                    msg.price,
                    msg.quantity,
                    msg.is_bid != 0
                );

                // --- RDTSC t2: just after LOB call ---
                uint64_t t2 = tsc_end();

				if (msg.order_id % 10000 == 0) {
   					 std::cerr << "[LOB] order=" << msg.order_id
            			  << " rescan bids=" << book.bids.rescan_count
            			  << " asks=" << book.asks.rescan_count << "\n";
				}
				

                // --- Build AckMsg ---
                ack.order_id = msg.order_id;
                ack.t1       = t1;
                ack.t2       = t2;
                ack.accepted = accepted ? 1 : 0;

                // --- Send AckMsg ---
                co_await asio::async_write(
                    client,
                    asio::buffer(&ack, hft::wire::AckMsg::SIZE),
                    use_awaitable);
            }
        } catch (const std::exception& e) {
            // EOF on disconnect is normal — only log unexpected errors
            std::string_view msg = e.what();
            if (msg.find("End of file") == std::string_view::npos &&
                msg.find("connection reset") == std::string_view::npos) {
                std::cerr << "[Client] Disconnected: " << e.what() << "\n";
            }
        }
    }

    asio::io_context& ctx_;
    tcp::acceptor     acceptor_;
};
