# cpp-hft-project

Low-latency HFT infra in C++20. Async TCP networking, a limit order book, rdtsc-based benchmarking, an optional kdb+/q tick store, and an ITCH 5.0 feed parser.

## Layout

    include/
      engine/
        order_book.hpp      — circular array LOB (ES futures)
        order.hpp, types.hpp, trades.hpp  — empty, reserved for later
      network/
        server.hpp          — ASIO C++20 coroutine TCP server, Nagle off
      protocol/
        messages.hpp        — binary wire protocol (fixed-size OrderMsg/AckMsg, ADD/CANCEL)
      analytics/
        kdb_client.hpp       — thin RAII wrapper around kdb+'s C API
      feed/
        itch_parser.hpp      — NASDAQ BX ITCH 5.0 parser
    src/
      main.cpp                     — io_context thread pool, wires up HFTServer
      benchmark/bench_client.cpp   — rdtsc latency bench, realistic add/cancel traffic
      analytics/kdb_client.cpp     — kdb_client.hpp impl (k.h stays isolated to this TU)
      feed/itch_stats.cpp          — itch_parser.hpp driver, prints a message-type histogram
      tests/order_book_test.cpp    — LOB correctness tests
      tests/kdb_test.cpp           — smoke test, needs a live q process
    third_party/kdb/
      k.h, c.o             — kdb+'s C API header + static object (grab separately, see Build)

## What's in it

### Async TCP server
ASIO C++20 coroutines (`co_await`/`co_spawn`), multi-threaded io_context pool, `TCP_NODELAY` so Nagle doesn't add latency, a work guard so the io_context doesn't exit early.

### Wire protocol
Fixed-size structs, no framing, no allocation, no strlen anywhere.
- `OrderMsg` (client→server): 64 bytes, one cache line. Carries a `type` byte (`ADD`/`CANCEL`) — added after an earlier all-adds version saturated the book (see benchmark notes below).
- `AckMsg` (server→client): 32 bytes. Carries the server's `t1`/`t2` rdtsc pair bracketing the LOB call, which is what makes the RTT-vs-processing split in the benchmark possible.
- `__attribute__((packed))` is there defensively — everything's already naturally aligned, it just documents the layout contract.

### Why the benchmark sends cancels, not just adds
Each price level caps out at `ORDERS_PER_LEVEL=8`. The first version of `bench_client` only sent adds, so with nothing freeing slots, every level saturated within ~5,000 messages — **~97% of a 100k-message run was just hitting the "level full, reject" path.** That means the p50/p99 numbers from that run were measuring rejection-loop cost, not actual insert/cancel/rescan work — not useful.

`bench_client` now tracks orders the server has actually confirmed resting (from real `ack.accepted` responses, not assumptions on the client side) and cancels against real live orders, keeping book depth oscillating in a ~150–300/side band via a force-add-below-half-target / force-cancel-at-target rule. That keeps `rescan_best()` and the FIFO/wraparound logic actually exercised instead of sitting idle.

### rdtsc latency benchmark
- Serialized timestamps: `lfence` + `rdtsc` at start, `rdtscp` at end.
- TSC calibrated against `steady_clock` — actual elapsed time, not the requested sleep duration.
- 1,000 warmup messages thrown away, 100,000 measured.
- Server brackets each LOB call with its own rdtsc pair (`t1` before the add/cancel, `t2` after) and hands both back in the AckMsg, so the client can split full RTT into:

      processing_ns = (t2 - t1) / tsc_ghz      # LOB only, server-side
      network_ns    = rtt_ns - processing_ns   # TCP send/recv + kernel scheduling

  Only valid because client and server share a physical CPU with `constant_tsc` verified — this wouldn't hold across machines.

- **Results** (Ryzen 7 7730U, loopback TCP, stock untuned kernel, 100k samples after 1k warmup, realistic add/cancel mix — ~50,750 adds / ~50,250 cancels, <0.1% legit reject rate):

  | Metric | RTT (full round trip) | LOB processing (server-side only) |
  |---|---|---|
  | Mean   |  16,359 ns | 52 ns |
  | p50    |  16,081 ns | 50 ns |
  | p90    |  18,295 ns | 70 ns |
  | p99    |  22,834 ns | ~280 ns |
  | p99.9  |  35,338 ns | ~510 ns |
  | Max    |  88,270 ns | ~4,200 ns (one outlier spike) |

  LOB processing is sub-100ns at the median, so the order book itself isn't the bottleneck — the ~16.3 µs of the ~16.4 µs RTT is loopback TCP/kernel overhead. TSC calibrated at ~1.996 GHz.

- **What "LOB processing" actually measures**: time inside `add_order()`/`cancel_order()` — array indexing, FIFO insert/remove, best-price rescan when it fires. Doesn't include TCP/kernel overhead, which is what dominates the RTT number above.

### OS tuning — what actually helped, tested not assumed
Ran the standard low-latency tuning checklist (`isolcpus`/`nohz_full`/`rcu_nocbs` core isolation, `taskset` pinning, performance governor, THP=`madvise`, swap off) against the benchmark instead of just applying it blindly. Headline: **on this workload, core isolation traded median latency for a tighter tail — it didn't just help across the board.**

| Config | Mean RTT | p50 | Max |
|---|---|---|---|
| Untuned baseline | 16.36 µs | 16.08 µs | 88.27 µs |
| isolcpus + nohz_full + rcu_nocbs (real separate cores) | 22.64 µs | 20.21 µs | 52.25 µs |
| isolcpus alone (nohz_full/rcu_nocbs removed) | 18.49 µs | 20.39 µs | **44.50 µs** |

Why: `nohz_full` only pays off if a core runs uninterrupted in userspace for a stretch, but this benchmark blocks on `write()`/`read_some()` every single message (every 16–20 µs) — too frequent for that condition to ever kick in, so it's probably paying `nohz_full`'s bookkeeping cost without getting the benefit. Isolation alone still costs ~4 µs at p50, plausibly from losing some locality trick the default scheduler was doing — but it buys a noticeably better worst-case tail. A real kernel-bypass design (spin-polling instead of blocking sockets) wouldn't eat this median cost at all, which is exactly why production HFT infra doesn't put blocking sockets on the hot path.

One config was tested with `isolcpus=6,7` before realizing those are two SMT sibling threads on the *same* physical core, not independent cores — that one regressed hard (max 139.99 µs) from L1/L2/front-end contention. Confirmed via `lscpu -e` and switched to actual non-sibling cores (6, 10) for the numbers above.

### Limit order book (ES futures)
- Circular array indexed by integer price ticks — O(1) add/cancel.
- `int64_t` price ticks throughout, zero floating point on the hot path.
- `DEPTH=1024` (power of 2) so modulo becomes a bitmask (`tick & 1023`).
- FIFO per price level — correct price-time priority.
- `Side<IsBid>` template kills bid/offer code duplication.
- Stale-slot detection on circular wraparound.
- Tested: insert, cancel, modify, price-time priority, wraparound.

### kdb+/q integration (optional)
`KdbClient` wraps kdb+'s C API (`k.h`) — the only interface KX ships for C/C++ — and it's touched exactly once, inside `kdb_client.cpp`. Everything else just sees `connect()`/`execute()`/`disconnect()`.

`k.h` predates const-correctness (`khpu`/`k` take non-const `S`), so `kdb_client.cpp` uses `const_cast` the way KX's own docs do it.

Fully decoupled — `hft_app`/LOB/benchmark build and run fine with zero kdb+ dependency. `kdb_test` is its own executable (used to accidentally share a `main()` with `hft_app` — fixed) and needs a live `q` process on `localhost:5001`. Without one it fails cleanly with `Failed to connect to q` (exit 1) — that's expected, not a bug.

### ITCH 5.0 feed parser
Parses raw NASDAQ BX TotalView-ITCH 5.0 binary feed files — the actual format real historical NASDAQ BX data ships in. `itch_stats` reads a file end to end and prints a per-message-type histogram plus total messages/bytes.

Tested against a real 3.28 GB historical BX ITCH file (March 2, 2020, ~109.4M messages): parsed clean, consumed all 3,280,036,325 bytes with zero remainder, and the 15 per-message-type counts sum exactly to the reported total. Spec-level checks also line up: exactly 6 System Event ('S') messages (fixed count per the ITCH spec — start/end of messages, system hours, market hours), exactly 1 MWCB Decline Level ('V') message (sent once a day), and Stock Directory ('R') count matching distinct symbol count exactly (8,909).

Also checked the Price Improvement Indicator ('N') message specifically against the official BX spec (section 4.7) — 20-byte fixed layout, unchanged from its introduction in July 2014 through this dataset's date and the current spec, confirmed via the spec's own revision log rather than just trusting the clean parse.

## Build

    mkdir build && cd build
    cmake ..
    make -j$(nproc)

    ./hft_app             # run server
    ./bench_client        # run benchmark, separate terminal
    ./order_book_test     # LOB unit tests
    ./itch_stats <path>   # parse a raw ITCH file, print histogram

    # kdb+/q smoke test (optional, needs kdb+/q + a running q process)
    # grab third_party/kdb/c.o from https://github.com/KxSystems/kdb/tree/master/l64
    # and k.h from https://github.com/KxSystems/kdb/blob/master/c/c/k.h
    q -p 5001   # separate terminal
    ./kdb_test

For low-latency experiments (see "OS tuning" above for what actually helped on this hardware):

    sudo cpupower frequency-set -g performance
    echo madvise | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
    sudo swapoff -a
    taskset -c <core> ./hft_app
    taskset -c <different-physical-core> ./bench_client

## Design notes

- No `-ffast-math` — breaks IEEE 754, too risky in financial math.
- No `using namespace` in headers — keeps TUs clean.
- CMake sources listed explicitly, no GLOB (cmake won't pick up new files without a re-run).
- Standard Linux TCP stack, no kernel bypass yet (DPDK is future work).
- The ~16 µs RTT vs ~50 ns LOB gap points straight at the kernel TCP stack as the bottleneck, not the order book — exactly what kernel bypass (DPDK/RDMA) would fix, and exactly why a blocking-socket design has the tuning ceiling it does above.

## Platform

- Ubuntu 24.04.4 LTS, bare metal, external 1TB drive, no hypervisor
- AMD Ryzen 7 7730U — Zen 3 "Barcelo-R", 8C/16T, 15W TDP (boosts ~55W), constant_tsc, nonstop_tsc, rdtscp, avx2
- GCC 13.3.0, C++20
- ASIO 1.24.0 standalone, no Boost

## Roadmap

- [ ] Matching engine (price-time priority, partial fills, IOC/FOK)
- [x] ITCH feed parser — validated against real 109.4M-message historical BX data, byte-exact
- [ ] Lock-free SPSC queue for the order pipeline
- [x] Core pinning/isolation, measured not assumed (see OS tuning)
- [x] kdb+/q C API integration (connect/execute/disconnect, isolated to one TU)
- [ ] DPDK / kernel bypass (kernel TCP stack is the confirmed bottleneck, per the RTT-vs-processing split above)
