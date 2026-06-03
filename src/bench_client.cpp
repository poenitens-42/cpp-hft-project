#include <asio.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <string_view>
#include <vector>
#include <cmath>

using asio::ip::tcp;

// rdtsc — Read Time Stamp Counter
//
// Returns CPU cycle count since last reset.


// Serialising read: lfence ensures all prior instructions retire
// before the counter is read. Use this for START of interval.
inline uint64_t rdtsc_start() {
    uint32_t lo, hi;
    __asm__ volatile (
        "lfence\n\t"       // serialise — drain out-of-order buffer
        "rdtsc\n\t"
        : "=a"(lo), "=d"(hi)
        :
        : "memory"         // compiler barrier — no reorder across this
    );
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// rdtscp for END of interval: implicitly serialises after the
// measured code completes, and gives us the core ID.
inline uint64_t rdtsc_end(uint32_t& core_id) {
    uint32_t lo, hi, aux;
    __asm__ volatile (
        "rdtscp\n\t"
        : "=a"(lo), "=d"(hi), "=c"(aux)
        :
        : "memory"
    );
    core_id = aux; // low 12 bits = core, bits 12-19 = socket
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// ------------------------------------------------------------
// TSC calibration
// We sleep for a known duration and measure TSC delta.
// cycles_per_ns = delta_cycles / sleep_ns
//
// NOTE: std::this_thread::sleep_for is not precise (OS scheduler
// wakes us late). We take the actual elapsed wall time via
// steady_clock, not the requested sleep duration.
// ------------------------------------------------------------
double calibrate_tsc_ghz() {
    using namespace std::chrono;

    auto wall_start = steady_clock::now();
    uint32_t dummy;
    uint64_t tsc_start = rdtsc_start();

    // Sleep long enough that OS imprecision is a small fraction
    std::this_thread::sleep_for(milliseconds(200));

    uint64_t tsc_end   = rdtsc_end(dummy);
    auto wall_end      = steady_clock::now();

    uint64_t tsc_delta  = tsc_end - tsc_start;
    double   ns_elapsed = duration_cast<nanoseconds>(wall_end - wall_start).count();

    return static_cast<double>(tsc_delta) / ns_elapsed; // cycles per ns = GHz
}

// ------------------------------------------------------------
// Percentile helper
// Input vector must be sorted.
// ------------------------------------------------------------
double percentile(const std::vector<double>& sorted_ns, double p) {
    if (sorted_ns.empty()) return 0.0;
    double idx = p / 100.0 * static_cast<double>(sorted_ns.size() - 1);
    std::size_t lo = static_cast<std::size_t>(idx);
    std::size_t hi = std::min(lo + 1, sorted_ns.size() - 1);
    double frac    = idx - static_cast<double>(lo);
    return sorted_ns[lo] + frac * (sorted_ns[hi] - sorted_ns[lo]);
}

// ------------------------------------------------------------
// Benchmark config
// ------------------------------------------------------------
static constexpr int    WARMUP_MSGS  = 1'000;   // discarded — let JIT/caches settle
static constexpr int    BENCH_MSGS   = 100'000;  // actual measurements
static constexpr int    TOTAL_MSGS   = WARMUP_MSGS + BENCH_MSGS;
static constexpr char   SERVER_IP[]  = "127.0.0.1";
static constexpr uint16_t SERVER_PORT = 9001;

// Payload — fixed size, no dynamic alloc in hot loop
// 64 bytes = one cache line, realistic for a small order message
static constexpr std::size_t MSG_SIZE = 64;
static constexpr char PAYLOAD[MSG_SIZE] = "BENCH_MSG_0000000000000000000000000000000000000000000000000000";

int main() {
    std::cout << "[Bench] Calibrating TSC...\n";
    double ghz = calibrate_tsc_ghz();
    std::cout << "[Bench] TSC frequency: " << std::fixed << std::setprecision(4)
              << ghz << " GHz\n";

    // Sanity check — if wildly off, something is wrong
    if (ghz < 0.5 || ghz > 6.0) {
        std::cerr << "[Bench] Calibration looks wrong — check constant_tsc in /proc/cpuinfo\n";
        return 1;
    }

    // ------------------------------------------------------------
    // Connect
    // ------------------------------------------------------------
    asio::io_context io;
    tcp::socket sock(io);
    tcp::resolver resolver(io);

    try {
        auto endpoints = resolver.resolve(SERVER_IP, std::to_string(SERVER_PORT));
        asio::connect(sock, endpoints);
        sock.set_option(tcp::no_delay(true)); // Nagle off — same as server
    } catch (const std::exception& e) {
        std::cerr << "[Bench] Connect failed: " << e.what()
                  << "\n  Is hft_app running on port " << SERVER_PORT << "?\n";
        return 1;
    }

    // Drain the server's welcome message
    {
        std::array<char, 256> tmp{};
        asio::error_code ec;
        sock.read_some(asio::buffer(tmp), ec);
    }

    std::cout << "[Bench] Connected. Running " << WARMUP_MSGS << " warmup + "
              << BENCH_MSGS << " measured messages...\n";

    // ------------------------------------------------------------
    // Hot loop
    // ------------------------------------------------------------
    std::vector<double> latencies_ns;
    latencies_ns.reserve(BENCH_MSGS);

    std::array<char, MSG_SIZE> recv_buf{};
    uint32_t core_id = 0;
    uint32_t core_id_start = 0;

    for (int i = 0; i < TOTAL_MSGS; ++i) {
        // --- START timestamp ---
        uint64_t t0 = rdtsc_start();
        // (lfence above ensures CPU doesn't speculate past this point)

        // Send
        asio::write(sock, asio::buffer(PAYLOAD, MSG_SIZE));

        // Receive echo (blocking — synchronous benchmark by design)
        // We want to measure one RTT at a time, not pipeline
        std::size_t received = 0;
        while (received < MSG_SIZE) {
            received += sock.read_some(asio::buffer(recv_buf.data() + received,
                                                     MSG_SIZE - received));
        }

        // --- END timestamp ---
        uint64_t t1 = rdtsc_end(core_id);

        if (i < WARMUP_MSGS) continue; // discard warmup

        // Convert cycles to nanoseconds
        double rtt_ns = static_cast<double>(t1 - t0) / ghz;
        latencies_ns.push_back(rtt_ns);

        // Warn if we migrated cores mid-measurement
        // (invalidates this sample — rdtsc not comparable across cores)
        if (core_id != core_id_start && i == WARMUP_MSGS) {
            core_id_start = core_id;
        }
    }

    sock.close();

    // ------------------------------------------------------------
    // Statistics
    // ------------------------------------------------------------
    std::sort(latencies_ns.begin(), latencies_ns.end());

    double sum  = std::accumulate(latencies_ns.begin(), latencies_ns.end(), 0.0);
    double mean = sum / static_cast<double>(latencies_ns.size());

    // Variance -> stddev
    double sq_sum = 0.0;
    for (double v : latencies_ns) sq_sum += (v - mean) * (v - mean);
    double stddev = std::sqrt(sq_sum / static_cast<double>(latencies_ns.size()));

    auto fmt = [](double ns) {
        return std::to_string(static_cast<int>(ns)) + " ns  ("
             + std::to_string(static_cast<int>(ns / 1000.0)) + " µs)";
    };

    std::cout << "\n========== RTT Latency Report ==========\n";
    std::cout << "  Samples   : " << latencies_ns.size()  << "\n";
    std::cout << "  Mean      : " << fmt(mean)             << "\n";
    std::cout << "  Std Dev   : " << fmt(stddev)           << "\n";
    std::cout << "  Min       : " << fmt(latencies_ns.front()) << "\n";
    std::cout << "  p50       : " << fmt(percentile(latencies_ns, 50.0))  << "\n";
    std::cout << "  p90       : " << fmt(percentile(latencies_ns, 90.0))  << "\n";
    std::cout << "  p99       : " << fmt(percentile(latencies_ns, 99.0))  << "\n";
    std::cout << "  p99.9     : " << fmt(percentile(latencies_ns, 99.9))  << "\n";
    std::cout << "  Max       : " << fmt(latencies_ns.back()) << "\n";
    std::cout << "=========================================\n";

    std::cout << "\n[Bench] TSC core at end: " << core_id << "\n";
    std::cout << "[Bench] Check /proc/cpuinfo for 'constant_tsc' and 'nonstop_tsc'\n";
    std::cout << "        If missing, TSC calibration is unreliable on this machine.\n";

    return 0;
}
