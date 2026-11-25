# limit-order-book

A single-instrument **limit order book and matching engine in C++20**, built for
low and predictable latency: price-time priority, five order types, O(1) cancel,
no heap allocation on the hot path, and a lock-free SPSC queue to feed it from a
gateway thread.

Correctness is checked by **differential fuzzing** against a deliberately naive
`std::map` order book: both books receive the same 800,000 random operations
and must emit identical event streams.

```
Throughput (single thread, 2,000,000 ops, 930,994 trades)
  OrderBook          11.26 M ops/s  (  88.8 ns/op)
  std::map book       2.19 M ops/s  ( 456.3 ns/op)
  speed-up             5.1x
```
<sub>AMD Ryzen 3 7320U laptop, WSL2 Ubuntu 22.04, g++ 11.4 `-O2 -march=native`. Full results below.</sub>

## Features

| | |
|---|---|
| **Matching** | Price-time (FIFO) priority; trades always print at the resting (maker) price |
| **Order types** | `Limit`, `Market`, `IOC`, `FOK` (all-or-nothing, no partial trades), `PostOnly` (rejected if it would take liquidity) |
| **Amend** | Reduce in place keeps queue priority; price change or size increase loses priority and may trade |
| **Cancel** | O(1), including from the middle of a queue |
| **Events** | accept / reject (with reason) / trade / cancel / modify, delivered to a compile-time listener |
| **Threading** | Lock-free single-producer/single-consumer ring buffer (`SpscRing`) to hand orders from a gateway thread to the matching thread |

## Design

```
                 bids_ (flat array, index = price - min_price)        asks_
   price idx:  ...  9995  9996  9997  9998 | 10002 10003 10004 ...
   level:            [ ]   [L]   [ ]   [L] | [L]   [ ]   [L]
                            |           |     |
                            v           v     v
   FIFO queue:            o->o        o->o->o  o            (intrusive doubly linked list,
                                                             32-bit pool slots)
   bitmap:       0     1     0     1   |  1     0     1     (+ summary bitmap over words)
```

**Price levels in flat arrays, not a tree.** Prices are integer ticks within a
configured band, so a level is found with one subtraction. A `std::map` costs a
pointer-chasing O(log n) walk and an allocation per new level.

**Two-level bitmap to find the next best price.** When the best level empties,
the engine must find the next non-empty one. Scanning tick by tick is O(gap);
`PriceBitmap` keeps one bit per level plus a summary bit per 64-bit word, so the
next level is found with `countr_zero` / `countl_zero`, and in the worst case a
scan of `levels / 4096` summary words.

**Intrusive FIFO lists over a fixed pool.** Orders are 32-byte nodes (two per
cache line) in a pre-allocated `Pool` and are linked by 32-bit indices rather
than pointers. Removing an order from the middle of a queue is O(1), and the
allocator is never called while matching.

**Engine-assigned sequential ids.** Order id -> pool slot is a `vector` lookup,
not a hash map. This mirrors real exchanges, where the venue assigns order ids.
Every `submit` consumes exactly one id, including rejected ones, so senders can
predict ids. The benchmark's gateway thread relies on this.

**Static dispatch for events.** The listener is a template parameter rather
than a virtual interface, so callbacks inline and `NullListener` compiles to
nothing.

**SPSC ring buffer.** Power-of-two capacity (mask instead of modulo), producer
and consumer state on separate cache lines to avoid false sharing,
acquire/release ordering with no CAS, and a cached copy of the other side's
index so the shared atomic is only re-read when the ring looks full or empty.

### Complexity

| operation | cost |
|---|---|
| submit (rests, no trade) | O(1) |
| submit that trades | O(fills + emptied levels × bitmap step) |
| cancel / reduce | O(1) amortised (bitmap step only if the best level empties) |
| best bid / ask | O(1) |
| depth(k) | O(k × bitmap step) |

### Trade-offs, stated honestly

- The price band is fixed at construction. Each level is 24 bytes per side, so
  a 200,000-tick band costs ~9.6 MB for both sides. A production engine would re-centre the
  window or fall back to a tree for far-away prices.
- `id_to_slot_` grows by 4 bytes per order ever submitted. A long-running
  engine would recycle id ranges.
- No self-trade prevention, iceberg orders or stop orders.

## Testing

```
21 tests, 0 failed
```

- **Unit tests** for every order type, priority rule, cancel/modify edge case,
  book-full behaviour and sparse-price recovery across bitmap word boundaries.
- **Differential fuzz test**: 40 seeds × 20,000 random operations in a narrow
  price band (so orders cross constantly), mixing all order types, cancels,
  modifies and invalid input. The fast book and `ReferenceBook` must produce
  the same events, the same best bid/ask after every operation and the same
  full depth. As a check that the oracle has teeth, a one-character bug in
  modify's priority rule was injected; it was caught at seed 17.
- **Bitmap** checked against `std::set` over 200,000 random operations.
- **SPSC ring** checked for ordering across two threads over 2M items.
- Clean under **AddressSanitizer + UndefinedBehaviorSanitizer** and
  **ThreadSanitizer**, with `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`
  and no warnings. CI runs all of these on GCC and Clang.

## Benchmarks

The synthetic flow is shaped like a liquid book: 55% passive limits a few ticks
from a random-walking mid (10% of them placed through the spread), 33% cancels,
5% amends, and 7% market/IOC takers.

```
Latency per operation in ns (timer overhead 34 ns subtracted)
  op            count      p50      p90      p99    p99.9
  submit      1239644       10      140      368     1920
  cancel       660295       75      216      432      704
  modify       100061       97      237      498      846

Pipeline (gateway thread -> SpscRing<4096> -> matching thread)
     11.21 M ops/s end to end, 930994 trades (expected 930994)
```

Notes on measurement:
- Per-operation timing uses `steady_clock`. Its ~34 ns overhead is measured and
  subtracted, so sub-50 ns figures are approximate. For nanosecond accuracy use
  `rdtsc` with a pinned core.
- Max latencies (not shown) reach about 1 ms. These are OS scheduling and
  first-touch page faults on a shared laptop VM, not the algorithm.
  Pre-faulting memory, core pinning and `isolcpus` are the usual fixes.
- The benchmark aborts if the fast and reference books disagree on trade count
  or volume.

## Build and run

Requires a C++20 compiler (GCC 11+, Clang 14+).

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/lob_tests                     # unit + fuzz tests
./build/lob_bench 2000000             # benchmarks
./build/lob_replay examples/session.txt
```

or without CMake: `make test`, `make bench`, `make demo`.

Sanitizer builds: `-DLOB_SANITIZE=ON` (ASan+UBSan) or `-DLOB_TSAN=ON`.

### Replay demo

```
> B 10102 90
  ACCEPT  #6 BUY LIMIT 90 @ 10102
  TRADE   50 @ 10100  maker #1  taker #6 (BUY aggressor)
  TRADE   30 @ 10100  maker #2  taker #6 (BUY aggressor)
> B 10105 500 FOK
  REJECT  #8 NOT_ENOUGH_LIQUIDITY
> MS 75
  ACCEPT  #9 SELL MARKET 75
  TRADE   10 @ 10102  maker #6  taker #9 (SELL aggressor)
  TRADE   20 @ 10090  maker #5  taker #9 (SELL aggressor)
  CANCEL  #9 qty 45
```

## Usage

```cpp
#include "lob/order_book.hpp"

struct MyListener : lob::NullListener {
  void on_trade(const lob::Trade& t) { /* publish to market data */ }
};

MyListener listener;
lob::OrderBook book({.min_price = 1, .max_price = 200'000, .max_orders = 1 << 20}, listener);

auto id = book.submit(lob::Side::Buy, 10'050, 100);          // limit
book.submit(lob::Side::Sell, 0, 30, lob::OrderType::Market); // market
book.modify(id, 10'050, 50);                                  // reduce, keeps priority
book.cancel(id);
```

## Layout

```
include/lob/order_book.hpp    matching engine
include/lob/price_bitmap.hpp  two-level bitmap for next/prev non-empty price
include/lob/pool.hpp          fixed-capacity slot allocator
include/lob/spsc_ring.hpp     lock-free SPSC queue
include/lob/types.hpp         ids, prices, order types, events
tests/                        unit tests, fuzz test, reference book
bench/                        workload generator and benchmarks
apps/replay.cpp               text-command replay tool
```

## License

MIT
