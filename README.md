# cpp-hft-project

A low-latency HFT infrastructure project in C++20, built around async networking, precision benchmarking, and a high-performance limit order book.

## Architecture

include/
server.hpp          — ASIO C++20 coroutine TCP server, Nagle disabled
order_book.hpp      — Circular array limit order book (ES futures)
src/
main.cpp            — io_context thread pool, HFTServer wiring
bench_client.cpp    — rdtsc latency benchmark client
order_book_test.cpp — LOB correctness tests



## Components

### Async TCP Server
- ASIO C++20 coroutines (co_await, co_spawn)
- Multi-threaded io_context pool (hardware_concurrency / 2)
- TCP_NODELAY — Nagle disabled, critical for tick-by-tick latency
- Work guard prevents premature io_context exit

### rdtsc Latency Benchmark
- Serialised timestamps: lfence + rdtsc at start, rdtscp at end
- TSC calibrated against steady_clock — actual elapsed, not requested sleep
- 1000 warmup messages discarded before recording
- Loopback results (AMD Ryzen 7 7730U):
  - p50:   15 us
  - p99:   23 us
  - p99.9: 32 us
  - Max:   144 us (with SCHED_FIFO RT priority)

### Limit Order Book (ES Futures)
- Circular array indexed by integer price ticks — O(1) add/cancel
- int64_t price ticks throughout — zero floating point in hot path
- DEPTH=1024 (power of 2) — modulo replaced by bitmask (tick & 1023)
- FIFO queue per price level — correct price-time priority
- Templated Side<IsBid> — eliminates bid/offer code duplication
- Stale slot detection on circular wraparound
- Tested: insert, cancel, modify, price-time priority, wraparound

## Build

```bash
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Run server
./hft_app

# Run benchmark (separate terminal)
./bench_client

# Run order book tests
./order_book_test
```

## Design Notes

- `-ffast-math` intentionally excluded — breaks IEEE 754, dangerous in financial math
- No `using namespace` in headers — prevents TU pollution
- Explicit CMake sources — no GLOB (cmake won't detect new files without re-run)
- All benchmarks run with `sudo chrt -f 50` for SCHED_FIFO RT priority

## Platform

- OS: Ubuntu 24.04.4 LTS (bare metal, external 1TB drive — no hypervisor)
- CPU: AMD Ryzen 7 7730U (constant_tsc, nonstop_tsc, rdtscp, avx2)
- Compiler: GCC 13.3.0, C++20
- Networking: ASIO 1.24.0 (standalone, no Boost)
