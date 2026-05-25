#pragma once

#include <asio.hpp>
#include <asio/experimental/awaitable_operators.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string_view>

// ------------------------------------------------------------
// Explicit imports — NEVER "using namespace" in a header.
// It leaks into every translation unit that includes this file,
// causing silent name collisions that are hell to debug.
// ------------------------------------------------------------
using asio::awaitable;
using asio::co_spawn;
using asio::detached;
using asio::use_awaitable;
using asio::ip::tcp;
namespace this_coro = asio::this_coro;

// Bring in ||, && operators for awaitables (used in composed ops)
using namespace asio::experimental::awaitable_operators; // OK inside .cpp, not ideal in .hpp
// TODO: move implementation to server.cpp to avoid this leaking

// ------------------------------------------------------------
// Message buffer sizing
// Fix: replaced raw char[256] with a typed array.
//   - std::array gives bounds safety and is zero-overhead vs raw array
//   - 4096 bytes fits most FIX/SBE market data messages
//   - For production: use a pre-allocated ring buffer (no heap alloc)
// ------------------------------------------------------------
static constexpr std::size_t kReadBufSize = 4096;

class HFTServer {
public:
    HFTServer(asio::io_context& ctx, uint16_t port)
        : ctx_(ctx),
          acceptor_(ctx, tcp::endpoint(tcp::v4(), port))
    {
        // SO_REUSEADDR is set by default in ASIO acceptor — good.
        // For production also consider: TCP_NODELAY on client sockets (Nagle off).
    }

    // Accept loop — runs until io_context is stopped
    awaitable<void> start() {
        // No std::cerr in hot path in production.
        // Here it's fine — accept loop is cold path.
        try {
            for (;;) {
                tcp::socket client = co_await acceptor_.async_accept(use_awaitable);

                // Disable Nagle's algorithm: critical for low-latency.
                // Nagle batches small packets — disastrous for HFT tick-by-tick data.
                client.set_option(tcp::no_delay(true));

                co_spawn(ctx_, handle_client(std::move(client)), detached);
                // NOTE: detached means exceptions thrown OUTSIDE our try/catch
                // in handle_client are silently dropped.
                // Production alternative: co_spawn(..., [](std::exception_ptr e){ ... })
            }
        } catch (const std::exception& e) {
            std::cerr << "[Server] Accept error: " << e.what() << "\n";
        }
    }

private:
    awaitable<void> handle_client(tcp::socket client) {
        try {
            // Greet client
            constexpr std::string_view welcome = "Connected to HFT async server\n";
            co_await asio::async_write(
                client, asio::buffer(welcome.data(), welcome.size()), use_awaitable);

            // Read buffer — stack allocated, fixed size, no heap
            std::array<char, kReadBufSize> buf{};

            for (;;) {
                std::size_t n = co_await client.async_read_some(
                    asio::buffer(buf), use_awaitable);

                // Fix: was std::cout (synchronous, mutex-locked) in hot path.
                // For production: use a lock-free spsc queue to a logger thread.
                // For now, keep it but be aware of the latency cost.
                std::string_view received{buf.data(), n};

                // Echo back — in a real feed handler you'd parse/dispatch here
                co_await asio::async_write(
                    client, asio::buffer(buf.data(), n), use_awaitable);
            }
        } catch (const asio::error_code& ec) {
            // EOF / connection reset — normal disconnect, not an error
            if (ec != asio::error::eof && ec != asio::error::connection_reset) {
                std::cerr << "[Client] Unexpected error: " << ec.message() << "\n";
            }
        } catch (const std::exception& e) {
            std::cerr << "[Client] Disconnected: " << e.what() << "\n";
        }
    }

    asio::io_context& ctx_;
    tcp::acceptor    acceptor_;
};
