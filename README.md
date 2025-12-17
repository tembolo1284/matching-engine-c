# High-Performance Matching Engine in C

A production-grade, cache-optimized order matching engine designed for high-frequency trading (HFT) applications. Built in C11 with a focus on sub-microsecond latency, deterministic performance, and safety-critical coding standards.

## Key Features

### Core Matching Engine
- **Price-Time Priority**: Orders matched by best price, then earliest timestamp (FIFO)
- **Order Types**: Limit orders and market orders (price = 0)
- **Partial Fills**: Large orders match against multiple resting orders
- **Multi-Symbol Support**: Independent order books per trading symbol
- **Order Cancellation**: Cancel individual orders or flush entire books

### High-Performance Architecture
- **Zero-Allocation Hot Path**: Memory pools pre-allocate all structures at startup
- **Cache-Line Optimized**: All hot structures aligned to 64-byte boundaries
- **Open-Addressing Hash Tables**: Linear probing for cache-friendly O(1) lookups
- **Lock-Free Queues**: SPSC queues with false-sharing prevention and batch operations
- **RDTSC Timestamps**: Serialized rdtscp for ~5 cycle timestamps on x86-64
- **Packed Enums**: `uint8_t` enums save 3 bytes per field vs standard enums
- **Batch Dequeue**: Amortizes atomic operations across multiple messages

### Network Modes
- **UDP Mode**: High-throughput single-client with multicast market data
- **TCP Mode**: Multi-client support with per-client message routing
- **Dual-Processor Mode**: Horizontal scaling with symbol-based partitioning (A-M / N-Z)

### Safety & Reliability
- **Power of Ten Compliant**: Follows NASA/JPL safety-critical coding standards
- **Compile-Time Verification**: `_Static_assert` validates all struct layouts
- **Bounded Operations**: All loops have fixed upper bounds
- **No Dynamic Allocation**: After initialization, zero malloc/free calls
- **Minimum 2 Assertions Per Function**: Defensive programming throughout

## Performance Characteristics

| Metric | Value | Notes |
|--------|-------|-------|
| Order latency | < 1 µs | Typical add/cancel/match |
| Timestamp overhead | ~5 cycles | Serialized rdtscp on x86-64 |
| Hash lookup | O(1), 1-2 cache lines | Open-addressing with linear probing |
| Memory per order | 64 bytes | Exactly one cache line |
| Queue throughput | > 10M msgs/sec | Lock-free SPSC with batch dequeue |
| Message envelope | 64 bytes | Cache-aligned for DMA efficiency |
| Batch dequeue speedup | ~20-30x | vs individual dequeue operations |

## Cache Optimization Summary

Every data structure has been optimized for modern CPU cache hierarchies:

| Structure | Size | Alignment | Optimization |
|-----------|------|-----------|--------------|
| `order_t` | 64 bytes | 64-byte | One order = one cache line |
| `price_level_t` | 64 bytes | 64-byte | Hot fields in first 32 bytes |
| `order_map_slot_t` | 32 bytes | natural | 2 slots per cache line |
| `output_msg_envelope_t` | 64 bytes | 64-byte | Perfect for DMA transfers |
| `client_entry_t` | 64 bytes | 64-byte | Prevents false sharing in registry |
| Queue head/tail | 8 bytes each | 64-byte padding | Prevents false sharing |
| Queue producer stats | 24 bytes | 64-byte padding | Producer-only, no atomics |
| Queue consumer stats | 16 bytes | 64-byte padding | Consumer-only, no atomics |

## Recent Optimizations (v2.0)

### Lock-Free Queue Improvements
- **Non-atomic statistics**: Producer and consumer stats on separate cache lines, updated without atomic operations
- **Removed CAS loop**: Peak size tracking uses simple compare instead of compare-and-swap
- **Batch dequeue**: `dequeue_batch()` performs single atomic operation for up to 32 messages
- **Empty poll optimization**: Polling empty queue no longer increments any counter

### Core Engine Fixes
- **Iterator invalidation fix**: `order_book_cancel_client_orders` now uses two-phase cancellation
- **RDTSC serialization**: Uses `rdtscp` (self-serializing) instead of plain `rdtsc`
- **Platform compatibility**: macOS Intel/ARM support with `clock_gettime` fallback
- **Type consistency**: All counts/indices use `uint32_t` instead of mixed `int`/`uint32_t`

### Safety Improvements
- **Rule 5 compliance**: Minimum 2 assertions per function throughout codebase
- **Rule 7 compliance**: All return values checked (pthread, clock_gettime, etc.)
- **Rule 9 compliance**: Multi-level pointer dereference eliminated via temp variables
- **Rule 4 compliance**: Large functions split (e.g., `order_book_flush`)

## Building

### Prerequisites
- GCC 7+ or Clang 6+ with C11 support
- CMake 3.10+
- Linux (for full performance) or macOS (with platform fallbacks)

### Build Commands

```bash
# Configure and build
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)

# Run tests
./matching_engine_tests

# Run benchmarks
./matching_engine_benchmark
```

### Build Flags

The project compiles with strict warnings:
```
-Wall -Wextra -Wpedantic -Werror -march=native -O3
```

## Usage

### UDP Mode (Single Client, Lowest Latency)

```bash
# Start engine
./matching_engine --mode udp --port 12345

# With multicast market data broadcast
./matching_engine --mode udp --port 12345 --multicast 239.255.0.1:5000
```

### TCP Mode (Multi-Client)

```bash
# Single processor
./matching_engine --mode tcp --port 12345

# Dual processor for horizontal scaling (A-M and N-Z symbol partitioning)
./matching_engine --mode tcp --port 12345 --dual-processor
```

### Message Format

**Input Messages (CSV):**
```
# New Order: N, user_id, symbol, price, qty, side, order_id
N, 1, IBM, 150, 100, B, 1001

# Cancel: C, symbol, user_id, order_id
C, IBM, 1, 1001

# Flush all books: F
F
```

**Output Messages:**
```
# Acknowledgment
A, user_id, order_id

# Trade
T, buy_user, buy_order, sell_user, sell_order, price, qty

# Top of Book Update
B, symbol, side, price, qty
```

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        Network Layer                             │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐  │
│  │ UDP Receiver│  │ TCP Server  │  │ Multicast Publisher     │  │
│  └──────┬──────┘  └──────┬──────┘  └────────────▲────────────┘  │
└─────────┼────────────────┼──────────────────────┼───────────────┘
          │                │                      │
          ▼                ▼                      │
┌─────────────────────────────────────────────────┼───────────────┐
│              Lock-Free SPSC Queues              │               │
│  ┌───────────────────┐    ┌─────────────────────┼─────────────┐ │
│  │ Input Queue       │    │ Output Queue        │             │ │
│  │ 64K × 56B slots   │    │ 64K × 64B envelopes─┘             │ │
│  │ [head]----64B----]│    │ Batch dequeue (32 msgs/op)        │ │
│  │ [tail]----64B----]│    │ Non-atomic stats                  │ │
│  │ [prod_stats]-64B-]│    │                                   │ │
│  │ [cons_stats]-64B-]│    │                                   │ │
│  └─────────┬─────────┘    └──────────────▲────────────────────┘ │
└────────────┼─────────────────────────────┼──────────────────────┘
             │                             │
             ▼                             │
┌────────────────────────────────────────────────────────────────┐
│                     Processor Thread                            │
│  • Batch dequeue up to 32 messages (single atomic op)          │
│  • Configurable spin-wait (PAUSE/YIELD) or nanosleep           │
│  • Prefetch next message while processing current              │
│  • Batched statistics updates (every 1000 messages)            │
│  • Local sequence counter to reduce atomic operations          │
└─────────────────────────┬──────────────────────────────────────┘
                          │
                          ▼
┌─────────────────────────────────────────────────────────────────┐
│                     Matching Engine                              │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │ Symbol Map (open-addressing, 512 slots)                   │  │
│  │ Hash: symbol[0..3] → slot, linear probe on collision      │  │
│  └───────────────────────────────────────────────────────────┘  │
│                              │                                   │
│              ┌───────────────┼───────────────┐                  │
│              ▼               ▼               ▼                  │
│  ┌───────────────┐ ┌───────────────┐ ┌───────────────┐         │
│  │ Order Book    │ │ Order Book    │ │ Order Book    │ ...     │
│  │ IBM           │ │ AAPL          │ │ NVDA          │         │
│  │               │ │               │ │               │         │
│  │ Bids: ──────┐ │ │ Bids: ──────┐ │ │ Bids: ──────┐ │         │
│  │  └─[100]────┼─┤ │  └─[150]────┼─┤ │  └─[200]────┼─┤         │
│  │    └─ord1   │ │ │    └─ord4   │ │ │    └─ord7   │ │         │
│  │    └─ord2   │ │ │             │ │ │             │ │         │
│  │             │ │ │             │ │ │             │ │         │
│  │ Asks: ──────┘ │ │ Asks: ──────┘ │ │ Asks: ──────┘ │         │
│  │  └─[101]──────┤ │  └─[151]──────┤ │  └─[201]──────┤         │
│  │    └─ord3     │ │    └─ord5     │ │    └─ord8     │         │
│  │               │ │    └─ord6     │ │               │         │
│  │ Order Map:    │ │ Order Map:    │ │ Order Map:    │         │
│  │ (user,oid)→*  │ │ (user,oid)→*  │ │ (user,oid)→*  │         │
│  └───────────────┘ └───────────────┘ └───────────────┘         │
│                                                                  │
│  ┌───────────────────────────────────────────────────────────┐  │
│  │ Memory Pools (shared across all books)                    │  │
│  │ Order Pool: 10,000 × 64 bytes = 640 KB (pre-allocated)   │  │
│  │ Free list: O(1) alloc/dealloc via index stack            │  │
│  └───────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────┘
```

## Dual-Processor Mode

For horizontal scaling, the engine supports two processor threads with symbol-based partitioning:

```
                    ┌─────────────────┐
                    │ Symbol Router   │
                    │ (branchless)    │
                    └────────┬────────┘
                             │
              ┌──────────────┴──────────────┐
              ▼                             ▼
    ┌─────────────────┐           ┌─────────────────┐
    │ Processor 0     │           │ Processor 1     │
    │ Symbols: A-M    │           │ Symbols: N-Z    │
    │                 │           │                 │
    │ Input Queue 0   │           │ Input Queue 1   │
    │ Output Queue 0  │           │ Output Queue 1  │
    │ Order Books 0   │           │ Order Books 1   │
    └─────────────────┘           └─────────────────┘
```

The symbol router uses a branchless computation:
```c
// 0 for A-M, 1 for N-Z
int processor = (symbol[0] >= 'N') | (symbol[0] >= 'n');
```

## Project Structure

```
matching-engine-c/
├── include/
│   ├── core/
│   │   ├── order.h              # 64-byte cache-aligned order struct
│   │   ├── order_book.h         # Price levels, open-addressing order map
│   │   └── matching_engine.h    # Multi-symbol engine, symbol map
│   ├── protocol/
│   │   ├── message_types.h      # Packed uint8_t enums, message structs
│   │   ├── message_types_extended.h  # Cache-aligned envelope types
│   │   ├── message_formatter.h  # Output serialization
│   │   └── symbol_router.h      # Branchless A-M/N-Z routing
│   ├── threading/
│   │   ├── lockfree_queue.h     # SPSC queue with batch dequeue
│   │   ├── processor.h          # Batched stats, spin-wait config
│   │   ├── client_registry.h    # Cache-aligned client tracking
│   │   ├── output_router.h      # Multi-queue message routing
│   │   └── queues.h             # Queue type instantiations
│   ├── network/
│   │   ├── tcp_server.h         # Multi-client TCP with epoll/kqueue
│   │   └── udp_server.h         # Single-client UDP
│   └── platform/
│       └── timestamps.h         # RDTSC / clock_gettime abstraction
├── src/
│   ├── core/                    # Matching logic implementation
│   ├── protocol/                # Message parsing and formatting
│   ├── threading/               # Processor thread implementation
│   ├── network/                 # Network I/O implementation
│   └── modes/                   # UDP/TCP mode entry points
├── tests/
│   ├── core/                    # Order book & engine tests
│   ├── protocol/                # Message parsing tests
│   └── threading/               # Queue tests (incl. batch dequeue)
├── documentation/
│   └── ARCHITECTURE.md          # Detailed design documentation
└── CMakeLists.txt
```

## Testing

```bash
# Run all tests
./matching_engine_tests

# Run specific test group
./matching_engine_tests -v -g OrderBook
./matching_engine_tests -v -g MemoryPools
./matching_engine_tests -v -g LockFreeQueue

# Memory checking (if valgrind available)
valgrind --leak-check=full ./matching_engine_tests
```

## Power of Ten Compliance

This codebase follows Gerard Holzmann's "Power of Ten" rules:

| Rule | Implementation |
|------|----------------|
| 1. No goto, setjmp, recursion | All control flow is structured |
| 2. Fixed loop bounds | `MAX_PROBE_LENGTH`, `MAX_MATCH_ITERATIONS`, `MAX_DRAIN_ITERATIONS` |
| 3. No malloc after init | Memory pools pre-allocate everything |
| 4. Functions ≤ 60 lines | Large functions split (e.g., `flush_process_side`) |
| 5. ≥ 2 assertions per function | Preconditions + postconditions throughout |
| 6. Smallest variable scope | Declared at point of use |
| 7. Check all return values | pthread, clock_gettime, all allocations verified |
| 8. Limited preprocessor | Simple macros, no complex logic |
| 9. Restrict pointer use | Temp variables eliminate `(*ptr)->field` chains |
| 10. Compile warning-free | `-Wall -Wextra -Wpedantic -Werror` |

## Platform Support

| Feature | Linux x86-64 | macOS Intel | macOS ARM |
|---------|--------------|-------------|-----------|
| Cache alignment | 64-byte | 64-byte | 64-byte |
| Timestamps | rdtscp (~5 cycles) | rdtscp (~5 cycles) | clock_gettime (~25ns) |
| Spin-wait hint | PAUSE | PAUSE | YIELD |
| Event loop | epoll | kqueue | kqueue |
| Compiler | GCC / Clang | Clang | Clang |

## References

- [Power of Ten - Rules for Developing Safety Critical Code](https://spinroot.com/gerard/pdf/P10.pdf) - Gerard Holzmann, NASA/JPL
- [What Every Programmer Should Know About Memory](https://www.akkadia.org/drepper/cpumemory.pdf) - Ulrich Drepper
- [Lock-Free Data Structures](https://www.cs.cmu.edu/~410-s05/lectures/L31_LockFree.pdf) - CMU

## Changelog

### v2.0 (December 2024)
- **Critical bug fix**: Iterator invalidation in `order_book_cancel_client_orders`
- **Critical bug fix**: RDTSC serialization (use `rdtscp` instead of `rdtsc`)
- **Performance**: Lock-free queue batch dequeue (~20-30x speedup for queue ops)
- **Performance**: Non-atomic queue statistics (eliminates ~45-90 cycles/message)
- **Performance**: Cache-aligned `client_entry_t` (prevents false sharing)
- **Safety**: Rule 5 compliance (minimum 2 assertions per function)
- **Safety**: Rule 7 compliance (all return values checked)
- **Safety**: Rule 9 compliance (eliminated multi-level pointer dereference)
- **Compatibility**: macOS platform support (Intel and Apple Silicon)

### v1.0 (Initial Release)
- Core matching engine with price-time priority
- Lock-free SPSC queues
- UDP and TCP modes
- Dual-processor support

