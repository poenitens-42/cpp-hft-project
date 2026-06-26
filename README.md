# cpp-hft-project

A low-latency HFT infrastructure project in C++20, built around async networking, precision benchmarking, and a high-performance limit order book.

## Architecture

    include/
      server.hpp          — ASIO C++20 coroutine TCP server, Nagle disabled
      order_book.hpp      — Circular array limit order book (ES futures)
      messages.hpp        — Binary wire protocol (fixed-size OrderMsg/AckMsg, no framing)
    src/
      main.cpp            — io_context thread pool, HFTServer wiring
      bench_client.cpp    — rdtsc latency benchmark client (RTT + LOB-only split)
      order_book_test.cpp — LOB correctness tests

## Components

### Async TCP Server
- ASIO C++20 coroutines (co_await, co_spawn)
- Multi-threaded io_context pool (hardware_concurrency / 2)
- TCP_NODELAY — Nagle disabled, critical for tick-by-tick latency
- Work guard prevents premature io_context exit

### Binary Wire Protocol
- Fixed-size structs, no framing, no dynamic allocation, no strlen
- `OrderMsg` (client→server): 64 bytes, one cache line
- `AckMsg` (server→client): 32 bytes, carries the server's `t1`/`t2`
  RDTSC pair bracketing the LOB call — this is what makes the
  RTT-vs-processing split in the benchmark possible
- `__attribute__((packed))`: defensive, since all fields are already
  naturally aligned — documents the layout contract explicitly

### rdtsc Latency Benchmark
- Serialised timestamps: lfence + rdtsc at start, rdtscp at end
- TSC calibrated against steady_clock — actual elapsed, not requested sleep
- 1000 warmup messages discarded, 100,000 measured
- Server brackets each LOB call with its own RDTSC pair (`t1` before
  `add_order()`, `t2` after) and returns both in the AckMsg. The client
  uses these to split full round-trip latency into two components:
processing_ns = (t2 - t1) / tsc_ghz      # LOB-only, server-side

network_ns    = rtt_ns - processing_ns    # TCP send/recv + kernel sched

Valid because client and server share the same physical CPU with
  `constant_tsc` verified — cross-machine TSC comparison would not be valid.

- Results (AMD Ryzen 7 7730U, loopback, `chrt -f 50` SCHED_FIFO,
  `taskset -c 4` server / `taskset -c 8` client, C-states disabled,
  performance governor, THP disabled, 100,000 samples after 1,000 warmup):

  | Metric | RTT (full round-trip) | LOB Processing (server-side only) |
  |---|---|---|
  | p50    |  21,280 ns  (21 µs) |   40 ns |
  | p90    |  22,452 ns  (22 µs) |   60 ns |
  | p99    |  26,109 ns  (26 µs) |  140 ns |
  | p99.9  |  44,194 ns  (44 µs) |  290 ns |
  | Max    | 363,784 ns (363 µs) | 4,438 ns (isolated spike) |

  RTT is dominated by loopback TCP stack overhead (~21 µs);
  LOB processing is sub-microsecond at all measured percentiles.
  TSC calibrated at 1.9962 GHz (Zen3 TSC ticks at a fixed reference
  frequency independent of boost clock — `constant_tsc` verified).
  Max LOB spike is a one-off scheduler preemption; rescan counters
  were 0 across all 100,000 orders (O(1) best-price fast path never
  fell back to linear scan).

- **What the LOB-processing number measures**: time inside
  `LimitOrderBook::add_order()` — array indexing, FIFO insert,
  best-price pointer update. Does **not** include TCP/kernel overhead,
  which dominates the RTT figure and is reported separately above.

### Limit Order Book (ES Futures)
- Circular array indexed by integer price ticks — O(1) add/cancel
- int64_t price ticks throughout — zero floating point in hot path
- DEPTH=1024 (power of 2) — modulo replaced by bitmask (tick & 1023)
- FIFO queue per price level — correct price-time priority
- Templated Side<IsBid> — eliminates bid/offer code duplication
- Stale slot detection on circular wraparound
- Tested: insert, cancel, modify, price-time priority, wraparound

## Build

    mkdir build && cd build
    cmake -DCMAKE_BUILD_TYPE=Release ..
    make -j$(nproc)

    # Run server (pinned, RT priority)
    sudo chrt -f 50 taskset -c 4 ./hft_app

    # Run benchmark (separate terminal, pinned)
    sudo chrt -f 50 taskset -c 8 ./bench_client

    # Run order book tests
    ./order_book_test

## Design Notes

- `-ffast-math` intentionally excluded — breaks IEEE 754, dangerous in financial math
- No `using namespace` in headers — prevents TU pollution
- Explicit CMake sources — no GLOB (cmake won't detect new files without re-run)
- All benchmarks run with `sudo chrt -f 50` for SCHED_FIFO RT priority
- Standard Linux TCP stack — no kernel bypass (DPDK noted as future work)
- The 21 µs RTT vs 40 ns LOB delta identifies the kernel TCP stack as the
  bottleneck, not the order book — which is exactly what kernel bypass (DPDK/RDMA)
  would target in a production system

## Platform

- OS: Ubuntu 24.04.4 LTS (bare metal, external 1TB drive — no hypervisor)
- CPU: AMD Ryzen 7 7730U (constant_tsc, nonstop_tsc, rdtscp, avx2)
- Compiler: GCC 13.3.0, C++20
- Networking: ASIO 1.24.0 (standalone, no Boost)

## Roadmap

- [ ] Matching engine (price-time priority, partial fills, IOC/FOK)
- [ ] Market data feed parser (ITCH protocol)
- [ ] Lock-free SPSC queue for order pipeline
- [x] Core pinning and NUMA-aware thread placement (taskset + SCHED_FIFO)
- [ ] kdb+/q tick store integration (latency logging + historical replay)
- [ ] DPDK kernel bypass (after kernel proven as bottleneck)
