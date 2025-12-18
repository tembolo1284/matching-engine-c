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
- **RDTSC Timestamps**: Serialized `rdtscp` for ~5 cycle timestamps on x86-64
- **Packed Enums**: `uint8_t` enums save 3 bytes per field vs standard enums
- **Batch Dequeue**: Amortizes atomic operations across up to 32 messages

### Network Layer
- **UDP Mode**: High-throughput single-client with multicast market data
- **TCP Mode**: Multi-client support with per-client message routing
- **Dual-Processor Mode**: Horizontal scaling with symbol-based partitioning (A-M / N-Z)
- **Low-Latency Sockets**: `TCP_NODELAY`, `TCP_QUICKACK`, `SO_BUSY_POLL` optimizations
- **Kernel Bypass Ready**: Abstraction points marked for DPDK/AF_XDP integration

### Protocol Support
- **CSV Protocol**: Human-readable format for debugging and testing
- **Binary Protocol**: Packed structs with compile-time size verification
- **Auto-Detection**: Per-client protocol detection (binary vs CSV)
- **Branchless Routing**: Symbol-based routing with zero conditional branches

### Safety & Reliability
- **Power of Ten Compliant**: Follows NASA/JPL safety-critical coding standards
- **Compile-Time Verification**: `_Static_assert` validates all struct layouts and offsets
- **Bounded Operations**: All loops have fixed upper bounds (Rule 2)
- **No Dynamic Allocation**: After initialization, zero malloc/free calls (Rule 3)
- **Defensive Programming**: Minimum 2 assertions per function (Rule 5)
- **Return Value Checking**: All system calls verified (Rule 7)

## Performance Characteristics

| Metric | Value | Notes |
|--------|-------|-------|
| Order latency | < 1 µs | Typical add/cancel/match |
| Timestamp overhead | ~5 cycles | Serialized `rdtscp` on x86-64 |
| Hash lookup | O(1), 1-2 cache lines | Open-addressing with linear probing |
| Memory per order | 64 bytes | Exactly one cache line |
| Queue throughput | > 10M msgs/sec | Lock-free SPSC with batch dequeue |
| Message envelope | 64 bytes | Cache-aligned for DMA efficiency |
| Batch dequeue speedup | ~20-30x | vs individual dequeue operations |
| Socket optimization | ~40 µs saved | `TCP_NODELAY` eliminates Nagle delay |

## Cache Optimization Summary

Every data structure has been optimized for modern CPU cache hierarchies:

| Structure | Size | Alignment | Optimization |
|-----------|------|-----------|--------------|
| `order_t` | 64 bytes | 64-byte | One order = one cache line |
| `price_level_t` | 64 bytes | 64-byte | Hot fields in first 32 bytes |
| `order_map_slot_t` | 32 bytes | natural | 2 slots per cache line |
| `output_msg_envelope_t` | 64 bytes | 64-byte | Perfect for DMA transfers |
| `client_entry_t` | 64 bytes | 64-byte | Prevents false sharing in registry |
| `udp_client_entry_t` | 32 bytes | natural | 2 entries per cache line |
| `tcp_client_t` | optimized | 64-byte | Hot fields grouped first |
| Queue head/tail | 8 bytes each | 64-byte padding | Prevents false sharing |
| Queue producer stats | 24 bytes | 64-byte padding | Producer-only, no atomics |
| Queue consumer stats | 16 bytes | 64-byte padding | Consumer-only, no atomics |

## Recent Optimizations (v2.1)

### Network Layer Enhancements
- **Socket optimizations**: `TCP_NODELAY`, `TCP_QUICKACK`, `SO_BUSY_POLL` for low latency
- **Thread-local formatters**: Eliminates global state contention in TCP/multicast
- **Kernel bypass preparation**: `[KB-1]` through `[KB-5]` abstraction markers throughout
- **32-byte UDP client entries**: Optimized hash table for client tracking
- **Rule 5/7 compliance**: 163 assertions across network layer

### Protocol Layer Improvements
- **Compile-time struct verification**: `_Static_assert` for all 7 binary protocol structs
- **Truly branchless routing**: `return (int)is_n_to_z` instead of ternary operator
- **Conditional debug logging**: `BINARY_PARSER_DEBUG` flag (off by default)
- **Validation helpers**: `side_is_valid()`, `symbol_is_valid()`, `client_id_is_valid()`
- **Offset assertions**: Verify field positions don't shift across compilers

### Lock-Free Queue Improvements
- **Non-atomic statistics**: Producer and consumer stats on separate cache lines
- **Removed CAS loop**: Peak size tracking uses simple compare instead of CAS
- **Batch dequeue**: `dequeue_batch()` performs single atomic for up to 32 messages
- **Empty poll optimization**: Polling empty queue no longer increments counters

### Core Engine Fixes
- **Iterator invalidation fix**: `order_book_cancel_client_orders` uses two-phase cancellation
- **RDTSC serialization**: Uses `rdtscp` (self-serializing) instead of plain `rdtsc`
- **Platform compatibility**: macOS Intel/ARM support with `clock_gettime` fallback
- **Type consistency**: All counts/indices use `uint32_t` instead of mixed types

### Safety Improvements
- **Rule 5 compliance**: Minimum 2 assertions per function (500+ assertions codebase-wide)
- **Rule 7 compliance**: All return values checked (`pthread_*`, `clock_gettime`, sockets)
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

### Build Options

```bash
# Enable DPDK kernel bypass (when implemented)
cmake .. -DUSE_DPDK=ON

# Enable debug assertions in release
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo

# Enable AddressSanitizer
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON
```

### Compiler Flags

The project compiles with strict warnings:
```
-Wall -Wextra -Wpedantic -Werror -march=native -O3 -flto
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

# Dual processor for horizontal scaling
./matching_engine --mode tcp --port 12345 --dual-processor

# With binary protocol output
./matching_engine --mode tcp --port 12345 --binary
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

**Input Messages (Binary):**
```
Magic (0x4D) | Type (1 byte) | Payload (variable)

New Order:  0x4D 0x01 [user_id:4][symbol:8][price:4][qty:4][side:1][order_id:4] = 27 bytes
Cancel:     0x4D 0x02 [user_id:4][order_id:4] = 10 bytes  
Flush:      0x4D 0x03 = 2 bytes
```

**Output Messages:**
```
# Acknowledgment
A, user_id, order_id

# Trade
T, buy_user, buy_order, sell_user, sell_order, price, qty, symbol

# Top of Book Update
B, symbol, side, price, qty

# Cancel Acknowledgment
X, user_id, order_id
```

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Network Layer                                │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────────┐  │
│  │ UDP Receiver    │  │ TCP Listener    │  │ Multicast Publisher │  │
│  │ • SO_BUSY_POLL  │  │ • TCP_NODELAY   │  │ • TTL config        │  │
│  │ • 10MB rx buf   │  │ • TCP_QUICKACK  │  │ • Batch send        │  │
│  │ • O(1) client   │  │ • epoll/kqueue  │  │ • Binary/CSV        │  │
│  │   hash table    │  │ • edge-trigger  │  │                     │  │
│  └────────┬────────┘  └────────┬────────┘  └──────────▲──────────┘  │
└───────────┼─────────────────────┼─────────────────────┼─────────────┘
            │                     │                     │
            ▼                     ▼                     │
┌───────────────────────────────────────────────────────┼─────────────┐
│                  Lock-Free SPSC Queues                │             │
│  ┌─────────────────────────────┐  ┌───────────────────┼───────────┐ │
│  │ Input Queue                 │  │ Output Queue      │           │ │
│  │ • 64K × 64B envelopes       │  │ • 64K × 64B       │           │ │
│  │ • Batch dequeue (32/op)     │  │ • Non-atomic stats│           │ │
│  │ • Cache-line aligned        │  │ • Round-robin     ◄───────────┘ │
│  │ • [head]────────────64B───] │  │   drain           │             │
│  │ • [tail]────────────64B───] │  │                   │             │
│  │ • [prod_stats]──────64B───] │  │                   │             │
│  │ • [cons_stats]──────64B───] │  │                   │             │
│  └──────────────┬──────────────┘  └───────────────────┘             │
└─────────────────┼───────────────────────────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      Processor Thread                                │
│  • Batch dequeue up to 32 messages (single atomic op)               │
│  • Configurable spin-wait (PAUSE/YIELD) or nanosleep                │
│  • Prefetch next message while processing current                   │
│  • Batched statistics updates (every 1000 messages)                 │
│  • Local sequence counter to reduce atomic operations               │
└──────────────────────────────┬──────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                       Matching Engine                                │
│                                                                      │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │ Symbol Map (open-addressing, 512 slots)                       │  │
│  │ Hash: symbol[0..3] → slot, linear probe on collision          │  │
│  │ _Static_assert verified size and alignment                    │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                              │                                       │
│              ┌───────────────┼───────────────┐                      │
│              ▼               ▼               ▼                      │
│  ┌───────────────┐ ┌───────────────┐ ┌───────────────┐             │
│  │ Order Book    │ │ Order Book    │ │ Order Book    │ ...         │
│  │ IBM           │ │ AAPL          │ │ NVDA          │             │
│  │               │ │               │ │               │             │
│  │ Bids: ──────┐ │ │ Bids: ──────┐ │ │ Bids: ──────┐ │             │
│  │  └─[100]────┼─┤ │  └─[150]────┼─┤ │  └─[200]────┼─┤             │
│  │    └─ord1   │ │ │    └─ord4   │ │ │    └─ord7   │ │             │
│  │    └─ord2   │ │ │             │ │ │             │ │             │
│  │             │ │ │             │ │ │             │ │             │
│  │ Asks: ──────┘ │ │ Asks: ──────┘ │ │ Asks: ──────┘ │             │
│  │  └─[101]──────┤ │  └─[151]──────┤ │  └─[201]──────┤             │
│  │    └─ord3     │ │    └─ord5     │ │    └─ord8     │             │
│  │               │ │    └─ord6     │ │               │             │
│  │ Order Map:    │ │ Order Map:    │ │ Order Map:    │             │
│  │ (user,oid)→*  │ │ (user,oid)→*  │ │ (user,oid)→*  │             │
│  │ 32B slots     │ │ 32B slots     │ │ 32B slots     │             │
│  └───────────────┘ └───────────────┘ └───────────────┘             │
│                                                                      │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │ Memory Pools (shared across all books)                        │  │
│  │ Order Pool: 10,000 × 64 bytes = 640 KB (pre-allocated)       │  │
│  │ Free list: O(1) alloc/dealloc via index stack                │  │
│  │ Zero malloc/free in hot path (Rule 3 compliant)              │  │
│  └───────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

## Dual-Processor Mode

For horizontal scaling, the engine supports two processor threads with symbol-based partitioning:

```
                    ┌──────────────────────┐
                    │ Symbol Router        │
                    │ Branchless:          │
                    │ return (int)is_n_to_z│
                    │ (no ternary/cmov)    │
                    └───────────┬──────────┘
                                │
              ┌─────────────────┴─────────────────┐
              ▼                                   ▼
    ┌─────────────────────┐             ┌─────────────────────┐
    │ Processor 0         │             │ Processor 1         │
    │ Symbols: A-M        │             │ Symbols: N-Z        │
    │                     │             │                     │
    │ Input Queue 0       │             │ Input Queue 1       │
    │ Output Queue 0      │             │ Output Queue 1      │
    │ Order Books 0       │             │ Order Books 1       │
    │                     │             │                     │
    │ Flush → BOTH queues │             │ Flush → BOTH queues │
    └─────────────────────┘             └─────────────────────┘
```

The symbol router uses a truly branchless computation:
```c
// Compile-time verification
_Static_assert(PROCESSOR_ID_A_TO_M == 0, "A-M must be 0");
_Static_assert(PROCESSOR_ID_N_TO_Z == 1, "N-Z must be 1");

// Branchless routing (no ternary, no cmov)
static inline int get_processor_id_for_symbol(const char* symbol) {
    assert(symbol != NULL && "NULL symbol");
    char first = symbol[0];
    bool is_n_to_z = (first >= 'N' && first <= 'Z') ||
                     (first >= 'n' && first <= 'z');
    int result = (int)is_n_to_z;  // Direct cast, truly branchless
    assert((result == 0 || result == 1) && "Invalid processor ID");
    return result;
}
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
│   │   ├── message_types.h      # Packed uint8_t enums, validation helpers
│   │   ├── message_types_extended.h  # Cache-aligned envelopes, debug helpers
│   │   ├── symbol_router.h      # Branchless A-M/N-Z routing
│   │   ├── binary/
│   │   │   ├── binary_protocol.h     # _Static_assert for all struct sizes
│   │   │   ├── binary_message_parser.h
│   │   │   └── binary_message_formatter.h
│   │   └── csv/
│   │       ├── message_parser.h      # Full strtoul error checking
│   │       └── message_formatter.h   # snprintf return value checks
│   ├── threading/
│   │   ├── lockfree_queue.h     # SPSC queue with batch dequeue
│   │   ├── processor.h          # Batched stats, spin-wait config
│   │   ├── client_registry.h    # 64-byte aligned client tracking
│   │   ├── output_router.h      # Multi-queue message routing
│   │   └── queues.h             # Queue type instantiations
│   ├── network/
│   │   ├── message_framing.h    # Length-prefixed TCP framing
│   │   ├── tcp_connection.h     # Per-client state, socket opts
│   │   ├── tcp_listener.h       # epoll/kqueue event loop
│   │   ├── udp_receiver.h       # O(1) client hash, bidirectional
│   │   └── multicast_publisher.h # TTL config, batch send
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
./matching_engine_tests -v -g BinaryProtocol

# Memory checking (if valgrind available)
valgrind --leak-check=full ./matching_engine_tests

# With AddressSanitizer
cmake .. -DENABLE_ASAN=ON && make && ./matching_engine_tests
```

## Power of Ten Compliance

This codebase follows Gerard Holzmann's "Power of Ten" rules:

| Rule | Implementation | Verification |
|------|----------------|--------------|
| 1. No goto, setjmp, recursion | All control flow is structured | Code review |
| 2. Fixed loop bounds | `MAX_PROBE_LENGTH`, `MAX_MATCH_ITERATIONS`, `MAX_DRAIN_ITERATIONS` | `_Static_assert` |
| 3. No malloc after init | Memory pools pre-allocate everything | Code review |
| 4. Functions ≤ 60 lines | Large functions split (e.g., `flush_process_side`) | `wc -l` |
| 5. ≥ 2 assertions per function | Preconditions + postconditions | `grep -c assert` |
| 6. Smallest variable scope | Declared at point of use | C99 style |
| 7. Check all return values | pthread, clock_gettime, socket ops | Code review |
| 8. Limited preprocessor | Simple macros, no complex logic | `grep -c '#define'` |
| 9. Restrict pointer use | Temp variables eliminate `(*ptr)->field` | Code review |
| 10. Compile warning-free | `-Wall -Wextra -Wpedantic -Werror` | CI build |

### Assertion Counts by Module

| Module | Assertions | Notes |
|--------|------------|-------|
| core/ | ~120 | order.h, order_book.c, matching_engine.c |
| threading/ | ~80 | lockfree_queue.h, processor.c |
| protocol/ | ~100 | parsers, formatters, validators |
| network/ | ~163 | All network layer files |
| **Total** | **~500+** | Minimum 2 per function |

## Socket Optimizations

The network layer applies aggressive low-latency socket options:

| Option | Effect | Latency Impact |
|--------|--------|----------------|
| `TCP_NODELAY` | Disable Nagle's algorithm | -40 µs typical |
| `TCP_QUICKACK` | Disable delayed ACKs (Linux) | -40 µs |
| `SO_BUSY_POLL` | Kernel busy polling (Linux) | -10-50 µs |
| `SO_REUSEPORT` | Multiple listeners | Scaling |
| `SO_RCVBUF` 10MB | Large receive buffer | Burst handling |
| `SO_SNDBUF` 4MB | Large send buffer | Burst handling |

## Kernel Bypass Preparation

The network layer includes abstraction points for future DPDK/AF_XDP integration:

```c
// Markers in code:
[KB-1] Socket setup     → DPDK port init
[KB-2] recvfrom()       → rte_eth_rx_burst()
[KB-3] sendto()         → rte_eth_tx_burst()
[KB-4] Client tracking  → Unchanged (hash table reusable)
[KB-5] accept()         → N/A for UDP, F-Stack for TCP
```

Proposed DPDK directory structure:
```
matching-engine-c/
├── include/network/          # Current (standard sockets)
├── include/network_dpdk/     # DPDK implementation
│   ├── dpdk_common.h         # Port/queue config
│   ├── dpdk_rx.h             # Receive burst wrapper
│   └── dpdk_tx.h             # Transmit burst wrapper
└── CMakeLists.txt            # USE_DPDK=ON flag
```

## Platform Support

| Feature | Linux x86-64 | macOS Intel | macOS ARM |
|---------|--------------|-------------|-----------|
| Cache alignment | 64-byte | 64-byte | 64-byte |
| Timestamps | `rdtscp` (~5 cycles) | `rdtscp` (~5 cycles) | `clock_gettime` (~25ns) |
| Spin-wait hint | `PAUSE` | `PAUSE` | `YIELD` |
| Event loop | epoll | kqueue | kqueue |
| `TCP_QUICKACK` | ✓ | ✗ | ✗ |
| `SO_BUSY_POLL` | ✓ (needs CAP_NET_ADMIN) | ✗ | ✗ |
| Compiler | GCC / Clang | Clang | Clang |

## References

- [Power of Ten - Rules for Developing Safety Critical Code](https://spinroot.com/gerard/pdf/P10.pdf) - Gerard Holzmann, NASA/JPL
- [What Every Programmer Should Know About Memory](https://www.akkadia.org/drepper/cpumemory.pdf) - Ulrich Drepper
- [Lock-Free Data Structures](https://www.cs.cmu.edu/~410-s05/lectures/L31_LockFree.pdf) - CMU
- [DPDK Programmer's Guide](https://doc.dpdk.org/guides/prog_guide/) - DPDK Project

## Changelog

### v2.1 (December 2024)
- **Network**: Socket optimizations (`TCP_NODELAY`, `TCP_QUICKACK`, `SO_BUSY_POLL`)
- **Network**: Thread-local parsers/formatters (eliminates global state)
- **Network**: Kernel bypass abstraction points (`[KB-1]` through `[KB-5]`)
- **Network**: 163 assertions for Rule 5 compliance
- **Protocol**: `_Static_assert` for all binary struct sizes and offsets
- **Protocol**: Truly branchless symbol routing (no ternary operator)
- **Protocol**: Conditional debug logging with `BINARY_PARSER_DEBUG` flag
- **Protocol**: Comprehensive validation helpers (`side_is_valid()`, etc.)

### v2.0 (December 2024)
- **Critical bug fix**: Iterator invalidation in `order_book_cancel_client_orders`
- **Critical bug fix**: RDTSC serialization (use `rdtscp` instead of `rdtsc`)
- **Performance**: Lock-free queue batch dequeue (~20-30x speedup)
- **Performance**: Non-atomic queue statistics (-45-90 cycles/message)
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

