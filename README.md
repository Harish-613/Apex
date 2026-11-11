# Apex

A limit order book and matching engine in C++20. It supports limit, market, stop and stop-limit orders, matches with price-time priority, and runs on a two-thread pipeline: a gateway thread feeds a matching thread through a lock-free single-producer/single-consumer ring buffer. Nothing allocates memory on the matching path.

```
gateway thread ──► SpscRing<Request> ──► matching thread ──► OrderBook ──► events (trades, cancels, ...)
   (producer)        lock-free, 64K slots       (consumer)       owns all book state, no locks
```

## Results

Measured on a shared 2-vCPU cloud VM (Intel Xeon @ 2.1 GHz, Ubuntu 24.04, GCC 13, `-O3`). Workload: 5M synthetic messages (55% limit, 25% cancel, 5% modify, 10% market, 5% stop) clustered around a drifting mid price. About 3M trades result.

| Benchmark | Result |
|---|---|
| Single-thread book throughput | **~10M messages/sec** |
| Single-thread `submit()` latency | p50 **170 ns**, p99 550 ns, p99.9 1.1 µs (includes ~30 ns timer overhead) |
| Two-thread pipeline, saturated | **~6.5M messages/sec** end to end |
| Two-thread pipeline at 200K msgs/sec, gateway → matched | p50 **~550 ns**, p90 ~800 ns |

**About tail latency.** On this VM the pipeline's p99 and above varied from 16 µs to 4 ms between identical runs. Both threads busy-spin on two shared vCPUs, so whenever the hypervisor or OS pauses either thread, the messages behind it wait. The median and p90 were stable across runs. To measure a meaningful tail, run on a machine with at least four physical cores, with the two threads pinned to isolated cores. On macOS, thread pinning isn't available, so expect a noisier tail there too.

Reproduce:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/bench_book              # single-thread throughput + per-message latency
./build/bench_pipeline          # two-thread throughput + paced end-to-end latency
python3 scripts/latency_report.py build/pipeline_latency.csv --plot latency.png
```

## Try it

```bash
printf "SELL 1 10 LIMIT 100\nSELL 2 5 LIMIT 101\nBUY 3 5 STOP 100\nBUY 4 12 LIMIT 100\nBOOK\n" | ./build/apex_cli
```

```
> BUY 4 12 LIMIT 100
  ACCEPTED  #4
  TRADE     10 @ 100  (taker #4 buy, maker #1)
  TRIGGERED #3
  TRADE     5 @ 101  (taker #3 buy, maker #2)
> BOOK
            ----------------
            bid    100  x 2
```

The trade at 100 triggers buy stop #3, which becomes a market order and lifts the 101 offer. The 2 unfilled lots of #4 rest as the new best bid.

Commands: `BUY|SELL <id> <qty> LIMIT <px>`, `... MARKET`, `... STOP <stop>`, `... STOPLIMIT <stop> <px>`, `CANCEL <id>`, `MODIFY <id> <px> <qty>`, `BOOK`.

## Matching rules

- **Price-time priority.** Better price fills first; at the same price, earlier arrival fills first.
- **Trades print at the maker's price.** A buy limit at 105 against a resting offer at 100 trades at 100.
- **Limit** orders match while prices cross, then rest any remainder. **Market** orders match at any price, and any remainder is cancelled.
- **Stop** orders wait off-book. A buy stop fires when the last trade is ≥ its stop price; a sell stop fires when the last trade is ≤ its stop price. A fired Stop becomes a Market order and a fired StopLimit becomes a Limit order. Stops are checked after an incoming order finishes matching. Fired orders run in FIFO order and can trigger more stops (a cascade). A stop placed when the market is already through it fires immediately.
- **Modify** applies to resting limit orders. Lowering the size at the same price keeps queue position. Changing the price or raising the size is a cancel/replace: the order goes to the back of the queue, and if the new price crosses the spread, it trades.

## Design walkthrough

Read the files in this order. Each one is short and commented.

### 1. `include/apex/types.hpp`: the data model
Prices are integer **ticks**, not doubles. That avoids rounding errors and lets a price index directly into an array. An `Order` is also an **intrusive linked-list node**: it carries its own `prev`/`next` pointers, so removing it from its price level is O(1) with no search and no separate node allocation.

### 2. `include/apex/order_book.hpp`: the book
- **Price levels as a flat array indexed by price** (`bids_[price]`, `asks_[price]`). Finding a level is a single array index, versus O(log n) for a `std::map` (red-black tree). The trade-off is a bounded price range, set by `BookConfig::price_levels`, which is 65,536 ticks by default.
- **Each level is a FIFO doubly-linked list.** New orders join at the tail and fills take from the head. That ordering *is* time priority.
- **Each level is `alignas(64)`** so it fills exactly one cache line. Updating one level never invalidates a neighbouring level's cache line.
- **Cached best bid and ask.** When the best level empties, `PriceBitset` finds the next non-empty level.
- **Cancel by id** goes through `OrderMap` to the `Order*`, then an O(1) unlink.

### 3. `include/apex/price_bitset.hpp`: finding the next price
There is one bit per tick, plus a summary word for each 64 words. To find the next set bit, the code checks the current word, jumps through the summary to the next non-empty word, and finishes with a single `countr_zero`/`countl_zero` instruction. One summary bit covers 4,096 ticks, so even a sparse book is searched in a few steps.

### 4. `include/apex/memory_pool.hpp`: no `new` on the hot path
All `Order` objects are preallocated. Allocate and free pop and push a **LIFO free list**, which costs a couple of pointer writes. That avoids `malloc` (unpredictable latency, possible lock contention). LIFO reuse also hands back the most recently freed slot, which is likely still in cache.

### 5. `include/apex/order_map.hpp`: id → order without allocation
This is an open-addressing hash table with linear probing, preallocated at twice the maximum order count (load factor ≤ 0.5). Probes walk adjacent memory instead of chasing `unordered_map` node pointers. Deletes use **backward-shift deletion**, so no tombstones pile up.

### 6. `include/apex/spsc_ring.hpp`: the lock-free queue
With one producer and one consumer, each index has exactly one writer, so no compare-and-swap is needed. Correctness comes from memory ordering:
- The producer writes the slot, *then* stores `tail` with `release`.
- The consumer loads `tail` with `acquire`, *then* reads the slot.

The release/acquire pair guarantees the consumer sees the fully written slot. The capacity is a **power of two**, so `index & mask` replaces modulo. The producer's fields and the consumer's fields sit on **separate cache lines** to avoid false sharing. Each side also caches the other side's index and only rereads the shared atomic when the ring looks full or empty.

### 7. `include/apex/engine.hpp`: threads
The matching thread owns the `OrderBook` exclusively, so the book needs no locks. The gateway calls `submit()`, which spins while the ring is full (backpressure). `stop()` enqueues a shutdown message behind all pending work, so everything already submitted is matched before the thread exits. On Linux, both threads are pinned to separate cores.

## Testing

`ctest --test-dir build` runs 27 tests:
- **Scenario tests** (`tests/test_order_book.cpp`) cover each matching rule: priority, partial fills, market sweeps, cancels, modifies, stop triggers and cascades, stop-limits, and rejects.
- **Component tests** (`tests/test_components.cpp`) check the pool, ring (including a 2M-message two-thread ordering test), bitset and hash map against brute-force or standard-library equivalents.
- **Differential test** (`tests/test_differential.cpp`) runs 400K random messages through Apex and through a slow, obviously correct reference book built on `std::map` and `std::deque`. Every trade, cancel, reject, stop trigger and the final book must match exactly. Injecting a one-character priority bug makes this test fail.
- CI runs the tests on Linux and macOS, plus a separate build under **AddressSanitizer and UndefinedBehaviorSanitizer**.

## Limitations and next steps

- Single instrument, with a bounded price range of 65,536 ticks by default. Supporting multiple symbols would mean one book per symbol, with symbol IDs routed by the gateway.
- No order-entry protocol (FIX, binary) and no network layer. Requests come from an in-process producer.
- Events are delivered through a compile-time listener rather than a second outbound ring.
- No self-trade prevention, iceberg orders, or time-in-force beyond GTC/IOC-like market behaviour.

## Layout

```
include/apex/   header-only library (types, pool, ring, bitset, map, book, engine, histogram)
tests/          scenario, component and differential tests (no external deps)
bench/          order-flow generator and the two benchmarks
apps/           apex_cli, a line-oriented demo front end
scripts/        latency_report.py: percentiles and CDF plot from benchmark CSVs
```
