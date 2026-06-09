#include <asio.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <random>
#include <string_view>
#include <vector>
#include <cmath>

#include "messages.hpp"

using asio::ip::tcp;

// ------------------------------------------------------------
// RDTSC — same as server side for apples-to-apples comparison
// lfence+rdtsc for start, rdtscp for end
// ------------------------------------------------------------
inline uint64_t rdtsc_start() {
    uint32_t lo, hi;
    __asm__ volatile (
        "lfence\n\t"
        "rdtsc\n\t"
        : "=a"(lo), "=d"(hi) :: "memory"
    );
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

inline uint64_t rdtsc_end(uint32_t& core_id) {
    uint32_t lo, hi, aux;
    __asm__ volatile (
        "rdtscp\n\t"
        : "=a"(lo), "=d"(hi), "=c"(aux) :: "memory"
    );
    core_id = aux;
    return (static_cast<uint64_t>(hi) << 32) | lo;
}

// ------------------------------------------------------------
// TSC calibration — unchanged
// ------------------------------------------------------------
double calibrate_tsc_ghz() {
    using namespace std::chrono;
    auto wall_start    = steady_clock::now();
    uint32_t dummy;
    uint64_t tsc_start_val = rdtsc_start();
    std::this_thread::sleep_for(milliseconds(200));
    uint64_t tsc_end_val   = rdtsc_end(dummy);
    auto wall_end      = steady_clock::now();
    uint64_t tsc_delta  = tsc_end_val - tsc_start_val;
    double   ns_elapsed = duration_cast<nanoseconds>(wall_end - wall_start).count();
    return static_cast<double>(tsc_delta) / ns_elapsed;
}

// ------------------------------------------------------------
// Percentile helper — input must be sorted
// ------------------------------------------------------------
double percentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    double idx = p / 100.0 * static_cast<double>(sorted.size() - 1);
    std::size_t lo = static_cast<std::size_t>(idx);
    std::size_t hi = std::min(lo + 1, sorted.size() - 1);
    double frac    = idx - static_cast<double>(lo);
    return sorted[lo] + frac * (sorted[hi] - sorted[lo]);
}

// ------------------------------------------------------------
// Randomised order generator
//
// Simulates realistic ES futures order flow:
//   - Price: random walk around 4500.00, ±50 ticks (±12.50 pts)
//   - Qty:   1-10 contracts (realistic retail/prop sizes)
//   - Side:  50/50 bid/ask
//
// We pre-generate all orders before the hot loop to avoid
// polluting latency measurements with RNG overhead.
// ------------------------------------------------------------
struct OrderGen {
    std::vector<hft::wire::OrderMsg> orders;

    explicit OrderGen(int n) {
        orders.reserve(n);

        std::mt19937_64 rng{42}; // fixed seed = reproducible runs
        // Price: 4480.00 to 4520.00 in 0.25 increments = 160 ticks
        // We pick tick offsets and convert to price
        std::uniform_int_distribution<int> tick_dist(-80, 80); // ±80 ticks from 4500
        std::uniform_int_distribution<int> qty_dist(1, 10);
        std::uniform_int_distribution<int> side_dist(0, 1);

        static constexpr double BASE_PRICE = 4500.00;
        static constexpr double TICK_SIZE  = 0.25;

        for (int i = 0; i < n; ++i) {
            hft::wire::OrderMsg msg{};
            msg.order_id = static_cast<uint64_t>(i + 1);
            msg.price    = BASE_PRICE + tick_dist(rng) * TICK_SIZE;
            msg.quantity = qty_dist(rng);
            msg.is_bid   = static_cast<uint8_t>(side_dist(rng));
            orders.push_back(msg);
        }
    }
};

// ------------------------------------------------------------
// Bench config
// ------------------------------------------------------------
static constexpr int      WARMUP_MSGS  = 1'000;
static constexpr int      BENCH_MSGS   = 100'000;
static constexpr int      TOTAL_MSGS   = WARMUP_MSGS + BENCH_MSGS;
static constexpr char     SERVER_IP[]  = "127.0.0.1";
static constexpr uint16_t SERVER_PORT  = 9001;

int main() {
    std::cout << "[Bench] Calibrating TSC...\n";
    double ghz = calibrate_tsc_ghz();
    std::cout << "[Bench] TSC frequency: " << std::fixed << std::setprecision(4)
              << ghz << " GHz\n";

    if (ghz < 0.5 || ghz > 6.0) {
        std::cerr << "[Bench] Calibration looks wrong — check constant_tsc\n";
        return 1;
    }

    // Pre-generate all orders — no RNG in hot loop
    std::cout << "[Bench] Pre-generating " << TOTAL_MSGS << " randomised orders...\n";
    OrderGen gen{TOTAL_MSGS};

    // ------------------------------------------------------------
    // Connect
    // ------------------------------------------------------------
    asio::io_context io;
    tcp::socket sock(io);
    tcp::resolver resolver(io);

    try {
        auto endpoints = resolver.resolve(SERVER_IP, std::to_string(SERVER_PORT));
        asio::connect(sock, endpoints);
        sock.set_option(tcp::no_delay(true));
    } catch (const std::exception& e) {
        std::cerr << "[Bench] Connect failed: " << e.what()
                  << "\n  Is hft_app running on port " << SERVER_PORT << "?\n";
        return 1;
    }

    // No welcome message to drain anymore — server sends nothing on connect

    std::cout << "[Bench] Connected. Running " << WARMUP_MSGS << " warmup + "
              << BENCH_MSGS << " measured messages...\n";

    // ------------------------------------------------------------
    // Hot loop
    // ------------------------------------------------------------
    std::vector<double> rtt_ns;
    std::vector<double> proc_ns;   // server-side LOB processing only
    std::vector<double> net_ns;    // RTT minus processing = network overhead
    rtt_ns.reserve(BENCH_MSGS);
    proc_ns.reserve(BENCH_MSGS);
    net_ns.reserve(BENCH_MSGS);

    std::array<char, hft::wire::AckMsg::SIZE> recv_buf{};
    hft::wire::AckMsg ack{};
    uint32_t core_id = 0;

    for (int i = 0; i < TOTAL_MSGS; ++i) {
        const auto& msg = gen.orders[i];

        // --- START RTT timestamp ---
        uint64_t t0 = rdtsc_start();

        // Send OrderMsg
        asio::write(sock, asio::buffer(&msg, hft::wire::OrderMsg::SIZE));

        // Receive AckMsg (exact size, loop until complete)
        std::size_t received = 0;
        while (received < hft::wire::AckMsg::SIZE) {
            received += sock.read_some(
                asio::buffer(recv_buf.data() + received,
                             hft::wire::AckMsg::SIZE - received));
        }

        // --- END RTT timestamp ---
        uint64_t t3 = rdtsc_end(core_id);

        if (i < WARMUP_MSGS) continue;

        // Parse ack
        std::memcpy(&ack, recv_buf.data(), sizeof(ack));

        // RTT = full round trip
        double rtt = static_cast<double>(t3 - t0) / ghz;

        // Processing = server-side LOB time (server's own TSC)
        // NOTE: server and client TSC are on the same physical machine,
        // same socket, constant_tsc verified — cross-process TSC comparison
        // is valid here. Would NOT be valid across machines.
        double proc = static_cast<double>(ack.t2 - ack.t1) / ghz;

        // Network overhead = RTT - processing
        // This includes: TCP send + loopback + TCP recv (both directions)
        // + any kernel scheduling between the two
        double net = rtt - proc;

        rtt_ns.push_back(rtt);
        proc_ns.push_back(proc);
        net_ns.push_back(net > 0.0 ? net : 0.0); // clamp negatives (TSC skew)
    }

    sock.close();

    // ------------------------------------------------------------
    // Statistics printer
    // ------------------------------------------------------------
    auto print_report = [](const std::string& label,
                           std::vector<double>& samples) {
        std::sort(samples.begin(), samples.end());
        double sum  = std::accumulate(samples.begin(), samples.end(), 0.0);
        double mean = sum / static_cast<double>(samples.size());
        double sq   = 0.0;
        for (double v : samples) sq += (v - mean) * (v - mean);
        double stddev = std::sqrt(sq / static_cast<double>(samples.size()));

        auto fmt = [](double ns) {
            return std::to_string(static_cast<int>(ns)) + " ns  ("
                 + std::to_string(static_cast<int>(ns / 1000.0)) + " µs)";
        };

        std::cout << "\n========== " << label << " ==========\n";
        std::cout << "  Samples   : " << samples.size()           << "\n";
        std::cout << "  Mean      : " << fmt(mean)                << "\n";
        std::cout << "  Std Dev   : " << fmt(stddev)              << "\n";
        std::cout << "  Min       : " << fmt(samples.front())     << "\n";
        std::cout << "  p50       : " << fmt(percentile(samples, 50.0))  << "\n";
        std::cout << "  p90       : " << fmt(percentile(samples, 90.0))  << "\n";
        std::cout << "  p99       : " << fmt(percentile(samples, 99.0))  << "\n";
        std::cout << "  p99.9     : " << fmt(percentile(samples, 99.9))  << "\n";
        std::cout << "  Max       : " << fmt(samples.back())      << "\n";
        std::cout << "===========================================\n";
    };

    print_report("RTT Latency (full round-trip)", rtt_ns);
    print_report("Processing Latency (LOB only, server-side)", proc_ns);
    print_report("Network Overhead (RTT - processing)", net_ns);

    std::cout << "\n[Bench] TSC core at end: " << core_id << "\n";
    std::cout << "[Bench] Note: processing latency uses server TSC directly.\n";
    std::cout << "        Valid because client+server share same physical CPU (constant_tsc verified).\n";

    return 0;
}
