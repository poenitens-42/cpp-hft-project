# cpp-hft-project

A low-latency HFT infrastructure project in C++20, built around async networking, precision benchmarking, a high-performance limit order book, and an optional kdb+/q tick-store integration.

## Architecture

    include/
      engine/
        order_book.hpp      — Circular array limit order book (ES futures)
        order.hpp, types.hpp, trades.hpp  — reserved, currently empty
      network/
        server.hpp          — ASIO C++20 coroutine TCP server, Nagle disabled
      protocol/
        messages.hpp        — Binary wire protocol (fixed-size OrderMsg/AckMsg, ADD/CANCEL)
      analytics/
        kdb_client.hpp       — thin RAII wrapper around kdb+'s C API
    src/
      main.cpp                     — io_context thread pool, HFTServer wiring
      benchmark/bench_client.cpp   — rdtsc latency benchmark, realistic add/cancel churn
      analytics/kdb_client.cpp     — kdb_client.hpp implementation (isolates k.h to this TU)
      tests/order_book_test.cpp    — LOB correctness tests
      tests/kdb_test.cpp           — standalone smoke test, needs a live q process
    third_party/kdb/
      k.h, c.o             — kdb+'s C API header + static object (download separately, see Build)

## Components

### Async TCP Server
- ASIO C++20 coroutines (co_await, co_spawn)
- Multi-threaded io_context pool
- TCP_NODELAY — Nagle disabled, critical for tick-by-tick latency
- Work guard prevents premature io_context exit

### Binary Wire Protocol
- Fixed-size structs, no framing, no dynamic allocation, no strlen
- `OrderMsg` (client→server): 64 bytes, one cache line — carries a `type`
  byte (`MsgType::ADD` / `MsgType::CANCEL`), added after discovering the
  original all-adds design saturated the book (see "Benchmark design" below)
- `AckMsg` (server→client): 32 bytes, carries the server's `t1`/`t2`
  RDTSC pair bracketing the LOB call — this is what makes the
  RTT-vs-processing split in the benchmark possible
- `__attribute__((packed))`: defensive, since all fields are already
  naturally aligned — documents the layout contract explicitly

### Benchmark design — why it generates cancels, not just adds
The order book caps each price level at `ORDERS_PER_LEVEL=8`. An earlier
version of `bench_client` only ever sent adds; with no way to free a slot,
every level saturated within ~5,000 messages and **~97% of a 100,000-message
run was hitting the "level full, reject" path**, not real order-book work —
p50/p99 latency numbers were measuring rejection-loop cost, not insertion,
cancellation, or best-price rescans.

`bench_client` now tracks which orders the server has actually confirmed as
resting (from real `ack.accepted` responses, not client-side assumptions),
and generates cancels against real, live orders — oscillating book depth in
a stable ~150–300/side band via a force-add-below-half-target /
force-cancel-at-target rule. This keeps `LimitOrderBook::rescan_best()` and
the FIFO/wraparound logic genuinely exercised instead of dormant.

### rdtsc Latency Benchmark
- Serialised timestamps: lfence + rdtsc at start, rdtscp at end
- TSC calibrated against steady_clock — actual elapsed, not requested sleep
- 1,000 warmup messages discarded, 100,000 measured
- Server brackets each LOB call with its own RDTSC pair (`t1` before the
  add/cancel call, `t2` after) and returns both in the AckMsg. The client
  uses these to split full round-trip latency into two components:

      processing_ns = (t2 - t1) / tsc_ghz      # LOB-only, server-side
      network_ns    = rtt_ns - processing_ns   # TCP send/recv + kernel sched

  Valid because client and server share the same physical CPU with
  `constant_tsc` verified — cross-machine TSC comparison would not be valid.

- **Results** (AMD Ryzen 7 7730U, loopback TCP, untuned stock kernel
  settings, 100,000 samples after 1,000 warmup, realistic add/cancel
  workload — ~50,750 adds / ~50,250 cancels, <0.1% legitimate reject rate):

  | Metric | RTT (full round-trip) | LOB Processing (server-side only) |
  |---|---|---|
  | Mean   |  16,359 ns  (16 µs) |   52 ns |
  | p50    |  16,081 ns  (16 µs) |   50 ns |
  | p90    |  18,295 ns  (18 µs) |   70 ns |
  | p99    |  22,834 ns  (23 µs) |  ~280 ns |
  | p99.9  |  35,338 ns  (35 µs) |  ~510 ns |
  | Max    |  88,270 ns  (88 µs) | ~4,200 ns (isolated spike) |

  LOB processing is sub-100ns at the median — the order book itself is not
  the bottleneck. RTT is dominated by loopback TCP/kernel overhead
  (~16.3 µs of the ~16.4 µs median RTT). TSC calibrated at ~1.996 GHz
  (`constant_tsc` verified).

- **What the LOB-processing number measures**: time inside
  `LimitOrderBook::add_order()`/`cancel_order()` — array indexing, FIFO
  insert/remove, best-price rescan when triggered. Does **not** include
  TCP/kernel overhead, which dominates the RTT figure above.

### OS Tuning — findings, not just a checklist
Standard low-latency tuning (core isolation via `isolcpus`/`nohz_full`/
`rcu_nocbs`, `taskset` pinning, `performance` governor, THP=`madvise`,
swap off) was tested against the benchmark above rather than applied
blindly. Headline finding: **for this workload, core isolation traded
median latency for tail predictability — it did not uniformly help.**

| Config | Mean RTT | p50 | Max |
|---|---|---|---|
| Untuned baseline | 16.36 µs | 16.08 µs | 88.27 µs |
| isolcpus + nohz_full + rcu_nocbs (on real separate cores) | 22.64 µs | 20.21 µs | 52.25 µs |
| isolcpus alone (nohz_full/rcu_nocbs removed) | 18.49 µs | 20.39 µs | **44.50 µs** |

Root cause: `nohz_full`'s tick-suspension benefit only pays off when a core
runs uninterrupted in userspace for a stretch — this benchmark does a
blocking `write()`/`read_some()` on every single message (~every 16–20 µs),
which is too frequent for that condition to ever engage, so it likely pays
`nohz_full`'s bookkeeping cost without reaping the benefit. Isolation alone
still carries a persistent ~4 µs p50 cost, plausibly from losing whatever
locality optimization the default (non-isolated) scheduler was providing —
but it buys a materially better, tighter worst-case tail. A real
kernel-bypass design (spin-polling instead of blocking sockets) wouldn't
pay this median cost at all — that's precisely why production HFT infra
avoids blocking sockets on the hot path in the first place. One config
was also tested with `isolcpus=6,7` — two SMT sibling threads of the same
physical core, not independent cores — which regressed further (max
139.99 µs) from direct L1/L2/front-end contention; confirmed via
`lscpu -e` and corrected to non-sibling cores (6, 10) for the results above.

### Limit Order Book (ES Futures)
- Circular array indexed by integer price ticks — O(1) add/cancel
- int64_t price ticks throughout — zero floating point in hot path
- DEPTH=1024 (power of 2) — modulo replaced by bitmask (tick & 1023)
- FIFO queue per price level — correct price-time priority
- Templated Side<IsBid> — eliminates bid/offer code duplication
- Stale slot detection on circular wraparound
- Tested: insert, cancel, modify, price-time priority, wraparound

### kdb+/q Integration (optional)
- `KdbClient` (`include/analytics/`, `src/analytics/`) wraps kdb+'s C API
  (`k.h`) — the only interface KX ships for C/C++, so it's touched exactly
  once, inside `kdb_client.cpp`; the rest of the codebase only sees
  `connect()`/`execute()`/`disconnect()`
- `k.h`'s API predates const-correctness (`khpu`/`k` take non-const `S`);
  `kdb_client.cpp` uses `const_cast` per KX's documented pattern
- Fully decoupled from `hft_app` — the core server/LOB/benchmark build and
  run with zero dependency on kdb+ being present
- `kdb_test` is a separate executable (previously mis-configured to share
  a `main()` with `hft_app` — fixed) and needs a live `q` process on
  `localhost:5001` to connect against; without one it fails cleanly with
  `Failed to connect to q` (exit 1), which is expected, not a bug

## Build

    mkdir build && cd build
    cmake ..
    make -j$(nproc)

    # Run server
    ./hft_app

    # Run benchmark (separate terminal)
    ./bench_client

    # Run order book unit tests
    ./order_book_test

    # Optional: kdb+/q smoke test (needs kdb+/q installed + a running q process)
    # download third_party/kdb/c.o from https://github.com/KxSystems/kdb/tree/master/l64
    # and k.h from https://github.com/KxSystems/kdb/blob/master/c/c/k.h
    q -p 5001   # in a separate terminal
    ./kdb_test

For low-latency experiments (optional — see "OS Tuning" above for what
actually helped vs. didn't on this hardware):

    sudo cpupower frequency-set -g performance
    echo madvise | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
    sudo swapoff -a
    taskset -c <core> ./hft_app
    taskset -c <different-physical-core> ./bench_client

## Design Notes

- `-ffast-math` intentionally excluded — breaks IEEE 754, dangerous in financial math
- No `using namespace` in headers — prevents TU pollution
- Explicit CMake sources — no GLOB (cmake won't detect new files without re-run)
- Standard Linux TCP stack — no kernel bypass (DPDK noted as future work)
- The ~16 µs RTT vs ~50 ns LOB delta identifies the kernel TCP stack as the
  bottleneck, not the order book — which is exactly what kernel bypass
  (DPDK/RDMA) would target in a production system, and exactly why a
  blocking-socket design's OS-tuning ceiling looks the way it does above

## Platform

- OS: Ubuntu 24.04.4 LTS (bare metal, external 1TB drive — no hypervisor)
- CPU: AMD Ryzen 7 7730U — Zen 3 "Barcelo-R", 8C/16T, 15W TDP (boosts ~55W)
  (constant_tsc, nonstop_tsc, rdtscp, avx2)
- Compiler: GCC 13.3.0, C++20
- Networking: ASIO 1.24.0 (standalone, no Boost)

## Roadmap

- [ ] Matching engine (price-time priority, partial fills, IOC/FOK)
- [ ] Market data feed parser (ITCH protocol)
- [ ] Lock-free SPSC queue for order pipeline
- [x] Core pinning and isolation, measured and compared (see "OS Tuning")
- [x] kdb+/q C API integration (connect/execute/disconnect, isolated to one TU)
- [ ] DPDK / kernel-bypass networking (the kernel TCP stack is the
      confirmed bottleneck per the RTT-vs-processing split above)
