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

#include "protocol/messages.hpp"

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
// RestingOrder — an order we believe is currently live on the
// server's book. Only populated from real ack.accepted==1
// responses, never assumed — the client's model of book state
// must track ground truth, not its own intent, or a cancel
// could reference an order the server actually rejected.
// ------------------------------------------------------------
struct RestingOrder {
    uint64_t order_id;
    double   price;
};

// ------------------------------------------------------------
// Bench config
// ------------------------------------------------------------
static constexpr int      WARMUP_MSGS  = 1'000;
static constexpr int      BENCH_MSGS   = 100'000;
static constexpr int      TOTAL_MSGS   = WARMUP_MSGS + BENCH_MSGS;
static constexpr char     SERVER_IP[]  = "127.0.0.1";
static constexpr uint16_t SERVER_PORT  = 9001;

// Target steady-state resting depth per side. Kept well under the
// hard per-side capacity (161 ticks * ORDERS_PER_LEVEL=8 = 1288)
// so per-tick collisions stay a rare, legitimate edge case rather
// than the dominant path. At target=300 across 161 ticks, average
// occupancy is ~1.9 orders/tick, so hitting a fully-loaded single
// tick (8/8) by chance is uncommon but not impossible — realistic.
static constexpr std::size_t TARGET_RESTING_PER_SIDE = 300;

static constexpr double   BASE_PRICE = 4500.00;
static constexpr double   TICK_SIZE  = 0.25;

int main() {
    std::cout << "[Bench] Calibrating TSC...\n";
    double ghz = calibrate_tsc_ghz();
    std::cout << "[Bench] TSC frequency: " << std::fixed << std::setprecision(4)
              << ghz << " GHz\n";

    if (ghz < 0.5 || ghz > 6.0) {
        std::cerr << "[Bench] Calibration looks wrong — check constant_tsc\n";
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
        sock.set_option(tcp::no_delay(true));
    } catch (const std::exception& e) {
        std::cerr << "[Bench] Connect failed: " << e.what()
                  << "\n  Is hft_app running on port " << SERVER_PORT << "?\n";
        return 1;
    }

    std::cout << "[Bench] Connected. Running " << WARMUP_MSGS << " warmup + "
              << BENCH_MSGS << " measured messages (add/cancel churn, target depth="
              << TARGET_RESTING_PER_SIDE << "/side)...\n";

    // ------------------------------------------------------------
    // Workload generator state
    //
    // NOTE: unlike the original all-adds version, this can't be
    // fully pre-generated — a CANCEL must reference an order the
    // server actually confirmed as resting, which is only known
    // at runtime from ack.accepted. The RNG draws + vector
    // bookkeeping below are O(1) and nanosecond-scale; negligible
    // next to microsecond-scale RTT, so they don't materially
    // pollute the latency measurement the way a full pre-generation
    // pass avoiding RNG entirely was originally trying to protect.
    // ------------------------------------------------------------
    std::mt19937_64 rng{42}; // fixed seed = reproducible runs
    std::uniform_int_distribution<int>    tick_dist(-80, 80);
    std::uniform_int_distribution<int>    qty_dist(1, 10);
    std::uniform_int_distribution<int>    side_dist(0, 1);
    std::uniform_real_distribution<double> action_roll(0.0, 1.0);

    std::vector<RestingOrder> resting_bids, resting_asks;
    resting_bids.reserve(TARGET_RESTING_PER_SIDE * 2);
    resting_asks.reserve(TARGET_RESTING_PER_SIDE * 2);

    uint64_t next_order_id = 1;
    uint64_t adds_sent = 0, adds_rejected = 0;
    uint64_t cancels_sent = 0, cancels_rejected = 0;

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
        bool is_bid = side_dist(rng) != 0;
        auto& resting = is_bid ? resting_bids : resting_asks;

        // Force add below half-target (bootstraps the book), force
        // cancel at/above target, coin-flip in between — keeps depth
        // oscillating in a stable band instead of drifting to either
        // extreme (which is what a fixed cancel-probability would do).
        bool is_cancel;
        if (resting.size() >= TARGET_RESTING_PER_SIDE) {
            is_cancel = true;
        } else if (resting.size() < TARGET_RESTING_PER_SIDE / 2) {
            is_cancel = false;
        } else {
            is_cancel = action_roll(rng) < 0.5;
        }

        hft::wire::OrderMsg msg{};
        std::size_t cancel_idx = 0; // only meaningful when is_cancel

        if (is_cancel) {
            std::uniform_int_distribution<std::size_t> pick(0, resting.size() - 1);
            cancel_idx = pick(rng);
            msg.order_id = resting[cancel_idx].order_id;
            msg.price    = resting[cancel_idx].price;
            msg.quantity = 0;
            msg.is_bid   = is_bid ? 1 : 0;
            msg.type     = static_cast<uint8_t>(hft::wire::MsgType::CANCEL);
        } else {
            msg.order_id = next_order_id++;
            msg.price    = BASE_PRICE + tick_dist(rng) * TICK_SIZE;
            msg.quantity = qty_dist(rng);
            msg.is_bid   = is_bid ? 1 : 0;
            msg.type     = static_cast<uint8_t>(hft::wire::MsgType::ADD);
        }

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

        // Parse ack — needed every iteration (including warmup) to
        // keep the resting-order model in sync with server ground truth.
        std::memcpy(&ack, recv_buf.data(), sizeof(ack));

        if (is_cancel) {
            ++cancels_sent;
            if (ack.accepted) {
                // swap-remove — order doesn't matter for a resting pool
                resting[cancel_idx] = resting.back();
                resting.pop_back();
            } else {
                ++cancels_rejected; // shouldn't normally happen — see note below
            }
        } else {
            ++adds_sent;
            if (ack.accepted) {
                resting.push_back({msg.order_id, msg.price});
            } else {
                ++adds_rejected; // legitimate: that specific tick's 8 slots are full
            }
        }

        if (i < WARMUP_MSGS) continue;

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

    std::cout << "\n========== Workload Composition ==========\n";
    std::cout << "  Adds    sent: " << adds_sent    << "  rejected: " << adds_rejected
               << "  (" << std::setprecision(1)
               << (100.0 * static_cast<double>(adds_rejected) / static_cast<double>(adds_sent))
               << "% — expected: rare, only when one specific tick's 8 slots are full)\n";
    std::cout << "  Cancels sent: " << cancels_sent << "  rejected: " << cancels_rejected
               << "  (should be ~0 — a non-zero count here means the client's resting-order\n"
               << "   model drifted from server truth and is worth investigating)\n";
    std::cout << "  Final resting depth — bids: " << resting_bids.size()
               << "  asks: " << resting_asks.size() << "\n";
    std::cout << "===========================================\n";

    std::cout << "\n[Bench] TSC core at end: " << core_id << "\n";
    std::cout << "[Bench] Note: processing latency uses server TSC directly.\n";
    std::cout << "        Valid because client+server share same physical CPU (constant_tsc verified).\n";

    return 0;
}
