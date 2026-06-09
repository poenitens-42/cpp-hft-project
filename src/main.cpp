#include "server.hpp"
#include <asio.hpp>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

int main() {
    const unsigned int num_threads = 2
    constexpr uint16_t PORT = 9001;

    try {
        asio::io_context io_ctx;
        auto work_guard = asio::make_work_guard(io_ctx);

        // Construct server BEFORE spawning threads — io_ctx fully initialized here
        HFTServer server(io_ctx, PORT);

        // Now spawn the coroutine — server already registered with io_ctx
        co_spawn(io_ctx, server.start(), asio::detached);

        std::cout << "[Main] Listening on port " << PORT
                  << " | threads: " << num_threads << "\n";
        std::vector<std::thread> pool;
        pool.reserve(num_threads);
        for (unsigned int i = 0; i < num_threads; ++i)
            pool.emplace_back([&io_ctx] { io_ctx.run(); });
        for (auto& t : pool) t.join();

    } catch (const std::exception& e) {
        std::cerr << "[Fatal] " << e.what() << "\n";
        return 1;
    }
    return 0;
}
