# Architecture

Comprehensive system design of the Matching Engine, emphasizing **zero-allocation memory pools**, lock-free threading, and production-grade architecture.

## Table of Contents
- [System Overview](#system-overview)
- [Memory Pool System](#memory-pool-system)
- [Threading Model](#threading-model)
- [Data Flow](#data-flow)
- [Core Components](#core-components)
- [Data Structures](#data-structures)
- [TCP Multi-Client Architecture](#tcp-multi-client-architecture)
- [C Port Details](#c-port-details)
- [Design Decisions](#design-decisions)
- [Performance Characteristics](#performance-characteristics)
- [Project Structure](#project-structure)

---

## System Overview

The Matching Engine is a **production-grade** order matching system built in pure C11. The defining characteristic is the **zero-allocation hot path** achieved through pre-allocated memory pools, eliminating malloc/free during order processing.

### High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────┐
│                        Matching Engine                                │
│                  Zero-Allocation Memory Pools                         │
├──────────────────────────────────────────────────────────────────────┤
│                                                                        │
│  ┌──────────────┐    ┌────────────┐    ┌──────────────┐            │
│  │   TCP/UDP    │───▶│ Lock-Free  │───▶│  Processor   │            │
│  │   Receiver   │    │   Queue    │    │  (Matching)  │            │
│  │  (Thread 1)  │    │ (16K msgs) │    │  (Thread 2)  │            │
│  └──────────────┘    └────────────┘    └──────────────┘            │
│         │                                       │                     │
│         │ Format Detection                      │                     │
│         │ (CSV/Binary)                          ▼                     │
│         │                              ┌────────────────┐            │
│         │                              │   Lock-Free    │            │
│         │                              │  Output Queue  │            │
│         │                              │  (16K msgs)    │            │
│         │                              └────────────────┘            │
│         │                                       │                     │
│         │                                       ▼                     │
│         │                              ┌───────────────┐             │
│         │                              │    Output     │             │
│         │                              │   Publisher   │             │
│         │                              │  (Thread 3)   │             │
│         │                              └───────────────┘             │
│         │                                       │                     │
│         └───────────────────────────────────────┘                    │
│                    (TCP per-client routing)                           │
│                                                                        │
└──────────────────────────────────────────────────────────────────────┘
```

**Key Design Principles:**
- **Zero malloc/free in hot path** - All memory pre-allocated
- **Lock-free communication** - SPSC queues between threads
- **Bounded loops** - Every loop has explicit iteration limits
- **Defensive programming** - Parameter validation, bounds checking
- **Client isolation** - TCP clients can't affect each other

---

## Memory Pool System

### Overview

The **memory pool system** is the cornerstone of this architecture. Instead of dynamic allocation (malloc/free), all memory is pre-allocated at startup and managed through O(1) index-based allocation.

### Why Memory Pools?

| Traditional malloc/free | Memory Pools |
|------------------------|--------------|
| Unpredictable latency (μs-ms) | Predictable latency (~5-10 CPU cycles) |
| Memory fragmentation over time | Zero fragmentation |
| System call overhead | Pure index manipulation |
| Allocation failures possible | Fail at startup (controlled) |
| Cache unfriendly (scattered) | Cache friendly (contiguous) |

### Pool Architecture

```c
/* All memory pools defined at compile time */
#define MAX_ORDERS_IN_POOL 10000
#define MAX_HASH_ENTRIES_IN_POOL 10000
#define MAX_SYMBOL_MAP_ENTRIES 512
#define MAX_ORDER_SYMBOL_ENTRIES 10000

typedef struct {
    /* Pre-allocated arrays */
    order_t orders[MAX_ORDERS_IN_POOL];
    order_map_entry_t hash_entries[MAX_HASH_ENTRIES_IN_POOL];
    
    /* Free list indices */
    uint32_t order_free_list[MAX_ORDERS_IN_POOL];
    uint32_t hash_free_list[MAX_HASH_ENTRIES_IN_POOL];
    
    /* Statistics */
    uint32_t peak_usage;
    uint32_t allocation_failures;
} memory_pools_t;
```

### Allocation Mechanism

```c
/* O(1) allocation - just pop from free list */
static inline order_t* order_pool_alloc(order_pool_t* pool) {
    if (pool->free_count == 0) {
        return NULL;  // Pool exhausted
    }
    
    // Pop index from free list
    pool->free_count--;
    uint32_t index = pool->free_list[pool->free_count];
    
    // Update statistics
    pool->total_allocations++;
    if ((MAX_ORDERS_IN_POOL - pool->free_count) > pool->peak_usage) {
        pool->peak_usage = MAX_ORDERS_IN_POOL - pool->free_count;
    }
    
    // Return pre-allocated order
    return &pool->orders[index];
}

/* O(1) deallocation - just push to free list */
static inline void order_pool_free(order_pool_t* pool, order_t* order) {
    uint32_t index = (uint32_t)(order - pool->orders);
    pool->free_list[pool->free_count] = index;
    pool->free_count++;
}
```

**Performance**: 
- Allocation: ~5-10 CPU cycles (vs 100-1000 for malloc)
- Zero system calls
- Zero lock contention
- Deterministic behavior

### Memory Pool Types

#### 1. Order Pool
```c
typedef struct {
    order_t orders[10000];           // Pre-allocated orders
    uint32_t free_list[10000];       // Available indices
    int free_count;                  // Number of free slots
    uint32_t peak_usage;             // Max simultaneous allocations
} order_pool_t;
```

**Used for**: Every order in the system

#### 2. Hash Entry Pool
```c
typedef struct {
    order_map_entry_t entries[10000];  // Hash table entries
    uint32_t free_list[10000];
    int free_count;
} hash_entry_pool_t;
```

**Used for**: Order lookup hash table (cancellations)

#### 3. Symbol Map Pool
```c
typedef struct {
    symbol_map_entry_t entries[512];   // Symbol → OrderBook mapping
    uint32_t free_list[512];
    int free_count;
} symbol_map_pool_t;
```

**Used for**: Matching engine symbol routing

#### 4. Order-Symbol Pool
```c
typedef struct {
    order_symbol_entry_t entries[10000];  // Order → Symbol mapping
    uint32_t free_list[10000];
    int free_count;
} order_symbol_pool_t;
```

**Used for**: Cancel operations (find which symbol an order belongs to)

### Memory Pool Initialization

```c
// One-time initialization at startup
void memory_pools_init(memory_pools_t* pools) {
    // Initialize all free lists
    for (int i = 0; i < MAX_ORDERS_IN_POOL; i++) {
        pools->order_pool.free_list[i] = i;
    }
    pools->order_pool.free_count = MAX_ORDERS_IN_POOL;
    
    // Similar for other pools...
}

// At startup
memory_pools_t global_pools;
memory_pools_init(&global_pools);

matching_engine_t engine;
matching_engine_init(&engine, &global_pools);  // Share pools
```

### Memory Pool Statistics

```c
typedef struct {
    uint32_t order_allocations;      // Total orders allocated
    uint32_t order_peak_usage;       // Max simultaneous orders
    uint32_t order_failures;         // Allocation failures
    uint32_t hash_allocations;
    uint32_t hash_peak_usage;
    uint32_t hash_failures;
    size_t total_memory_bytes;       // Total pre-allocated memory
} memory_pool_stats_t;

// Query at any time
memory_pool_stats_t stats;
memory_pools_get_stats(&global_pools, &stats);
```

**Typical Usage**:
- Peak usage: 500-2000 orders
- Memory: ~10-20MB pre-allocated
- Failures: 0 (unless pool exhausted)

---

## Threading Model

### Three-Thread Pipeline

**Thread 1: TCP/UDP Receiver**
- Handles network I/O (epoll for TCP, recvfrom for UDP)
- Detects message format (CSV vs Binary)
- Parses messages into internal format
- Enqueues to input queue (lock-free write)

**Thread 2: Processor**
- Dequeues messages in batches (up to 32)
- Routes to appropriate order book (by symbol)
- Executes matching logic (price-time priority)
- Allocates orders/hash entries from pools (NO malloc!)
- Generates output messages
- Enqueues to output queue (lock-free read/write)

**Thread 3: Output Publisher**
- Dequeues output messages
- Formats as CSV or Binary
- Routes to appropriate client(s) in TCP mode
- Publishes to stdout or TCP sockets (lock-free read)

### Lock-Free Communication

```c
// Single Producer, Single Consumer (SPSC) Queue
typedef struct {
    _Alignas(64) atomic_uint_fast64_t read_pos;   // Consumer cache line
    _Alignas(64) atomic_uint_fast64_t write_pos;  // Producer cache line
    _Alignas(64) input_msg_t data[16384];         // Data cache lines
} input_queue_t;
```

**Key Properties:**
- Cache-line aligned (64 bytes) prevents false sharing
- Atomic operations for synchronization (no locks!)
- No contention between threads
- Typical latency: 100-500ns per hop

---

## Data Flow

### End-to-End Flow

```
1. TCP Client sends: "N, 1, IBM, 100, 50, B, 1"
2. TCP Listener (epoll) receives message
3. Parse CSV → input_msg_t
4. Enqueue to input_queue (lock-free)
   
5. Processor dequeues (batch of 32)
6. Route to IBM order book
7. Allocate order from pool (O(1), no malloc!)
8. Match order → Generate trade
9. Enqueue output_msg_t[] to output_queue
   
10. Output Publisher dequeues
11. Route to appropriate TCP client socket(s)
12. Both clients receive trade notification
```

### TCP Multi-Client Data Flow

```
Client 1 (Buy) ───┐
                  ├──▶ TCP Listener ──▶ Input Queue ──▶ Processor ──▶ Output Router
Client 2 (Sell) ──┘         │                              │               │
                           │                              │               │
                           │                              │               ▼
                           │                              │        ┌────────────┐
                           │                              │        │ Client 1 Q │
                           │                              │        │ Client 2 Q │
                           │                              │        └────────────┘
                           │                              │               │
                           └──────────────────────────────┴───────────────┘
                                    (Trade sent to both clients)
```

---

## Core Components

### Order Book (`src/core/order_book.c`)

**Responsibilities:**
- Maintain price levels (buy/sell sides)
- Match incoming orders (price-time priority)
- Track best bid/ask (top-of-book)
- Generate output messages (acks, trades, TOB updates)
- Use memory pools for all allocations

**Key Data Structures:**
```c
typedef struct {
    char symbol[MAX_SYMBOL_LENGTH];
    
    /* Price levels - fixed arrays */
    price_level_t bids[MAX_PRICE_LEVELS];   // Descending price order
    price_level_t asks[MAX_PRICE_LEVELS];   // Ascending price order
    int num_bid_levels;
    int num_ask_levels;
    
    /* Order lookup for cancellations */
    order_map_t order_map;
    
    /* Memory pools (shared reference) */
    memory_pools_t* pools;
} order_book_t;
```

**Matching Algorithm:**
1. Allocate order from pool
2. Try to match against opposite side
3. If fully matched → free order back to pool
4. If partially matched → add remainder to book
5. Update top-of-book if needed

### Matching Engine (`src/core/matching_engine.c`)

**Responsibilities:**
- Route messages to correct order book (by symbol)
- Create order books on-demand
- Track order→symbol mapping (for cancellations)
- Cancel all orders for disconnected TCP clients
- Manage engine-specific memory pools

**Key Data Structures:**
```c
typedef struct {
    /* Symbol → OrderBook mapping */
    symbol_map_entry_t* symbol_map[SYMBOL_MAP_SIZE];
    
    /* Order → Symbol mapping (for cancellations) */
    order_symbol_entry_t* order_to_symbol[ORDER_SYMBOL_MAP_SIZE];
    
    /* Pre-allocated order books */
    order_book_t books[MAX_SYMBOLS];
    int num_books;
    
    /* Shared memory pools */
    memory_pools_t* pools;
    
    /* Engine-specific pools */
    symbol_map_pool_t symbol_pool;
    order_symbol_pool_t order_symbol_pool;
} matching_engine_t;
```

### Price Level Management

**Binary Search on Sorted Array:**
```c
// Find price level (O(log N))
int find_price_level(const price_level_t* levels, int num_levels, 
                     uint32_t price, bool descending);

// Insert maintaining sort order
int insert_price_level(price_level_t* levels, int* num_levels, 
                       uint32_t price, bool descending);
```

**Why not hash table for prices?**
- O(log N) is fast for N < 10,000
- Maintains natural price ordering
- Better cache locality
- Simpler implementation

---

## Data Structures

### Order Structure

```c
typedef struct order {
    /* Identification */
    uint32_t user_id;
    uint32_t user_order_id;
    char symbol[MAX_SYMBOL_LENGTH];
    
    /* Order details */
    uint32_t price;           // 0 = market order
    uint32_t quantity;
    uint32_t remaining_qty;   // For partial fills
    side_t side;              // BUY or SELL
    order_type_t type;        // MARKET or LIMIT
    
    /* Time priority */
    uint64_t timestamp;       // Nanoseconds since epoch
    
    /* TCP multi-client support */
    uint32_t client_id;       // 0 = UDP, >0 = TCP client
    
    /* Doubly-linked list */
    struct order* next;
    struct order* prev;
} order_t;
```

### Price Level

```c
typedef struct {
    uint32_t price;
    uint32_t total_quantity;     // Sum of all orders at this price
    order_t* orders_head;        // FIFO list for time priority
    order_t* orders_tail;
    bool active;
} price_level_t;
```

### Output Buffer

```c
typedef struct {
    output_msg_t messages[MAX_OUTPUT_MESSAGES];
    int count;
} output_buffer_t;

// Helper functions
static inline void output_buffer_init(output_buffer_t* buf) {
    buf->count = 0;
}

static inline void output_buffer_add(output_buffer_t* buf, const output_msg_t* msg) {
    if (buf->count < MAX_OUTPUT_MESSAGES) {
        buf->messages[buf->count++] = *msg;
    }
}
```

---

## TCP Multi-Client Architecture

### Client Connection Management

```c
// Each TCP client gets unique ID
typedef struct {
    int socket_fd;
    uint32_t client_id;          // 1-based
    bool active;
    char read_buffer[BUFFER_SIZE];
    size_t read_offset;
} tcp_client_t;
```

### Client Isolation

**Order Ownership:**
```c
// When placing order
order->client_id = client_id;

// Validation: user_id must match client_id
if (msg->user_id != client_id) {
    fprintf(stderr, "User ID mismatch: msg=%u, client=%u\n",
            msg->user_id, client_id);
    return;
}
```

**Auto-Cancel on Disconnect:**
```c
// When client disconnects
size_t cancelled = matching_engine_cancel_client_orders(engine, 
                                                        client_id, 
                                                        &output);
fprintf(stderr, "Client %u disconnected, cancelled %zu orders\n",
        client_id, cancelled);
```

### Message Routing

**Broadcast to Affected Clients:**
```c
// Trade affects both buyer and seller
if (msg->type == OUTPUT_MSG_TRADE) {
    uint32_t buy_client = msg->data.trade.buy_client_id;
    uint32_t sell_client = msg->data.trade.sell_client_id;
    
    send_to_client(buy_client, msg);
    send_to_client(sell_client, msg);
}
```

**Unicast for Others:**
```c
// Ack, cancel ack → only to originating client
if (msg->type == OUTPUT_MSG_ACK || msg->type == OUTPUT_MSG_CANCEL_ACK) {
    send_to_client(msg->client_id, msg);
}
```

---

## C Port Details

### How We Replaced C++ Features

| C++ Feature | C Implementation | Files |
|-------------|------------------|-------|
| `std::vector<Order*>` + `new`/`delete` | Memory pools | `core/order_book.c` |
| `std::variant<A,B,C>` | Tagged unions | `protocol/message_types.h` |
| `std::optional<T>` | Bool + output param | Various |
| `std::string` | Fixed `char[16]` | `core/order.h` |
| `std::map<price, orders>` | Binary search on sorted array | `core/order_book.c` |
| `std::unordered_map<order_id, order*>` | Hash table + pool | `core/order_book.c` |
| `std::thread` | pthreads | `threading/*` |
| `std::atomic` | C11 `<stdatomic.h>` | `threading/lockfree_queue.h` |
| `std::chrono` | `clock_gettime()` | `core/order.h` |
| Templates | C macros | `threading/lockfree_queue.h` |

### Memory Management Patterns

**C++ Way:**
```cpp
Order* order = new Order();  // Heap allocation
book.add(order);
// ... later ...
delete order;                // Heap deallocation
```

**Our C Way:**
```c
order_t* order = order_pool_alloc(&pools->order_pool);  // O(1) from pool
order_book_add_order(book, order, output);
// ... later ...
order_pool_free(&pools->order_pool, order);             // O(1) back to pool
```

**Benefits:**
- Predictable latency
- Zero fragmentation
- No system calls
- Cache friendly

---

## Design Decisions

### 1. Memory Pools vs Malloc/Free

**Decision:** Pre-allocate all memory in pools

**Rationale:**
- **Predictable latency** - No unpredictable system calls
- **Zero fragmentation** - Memory stays contiguous
- **Production-grade** - Real exchanges use this pattern
- **Fail fast** - Pool exhaustion detected immediately
- **Statistics** - Track peak usage, failures

**Trade-off:** Fixed maximum capacity (10K orders, etc.)

### 2. Binary Search vs Hash Table for Prices

**Decision:** Binary search on sorted array

**Rationale:**
- O(log N) is fast for N < 10,000 (~13 comparisons max)
- Maintains natural price ordering
- Better cache performance than trees
- Simpler implementation
- Modern CPUs make small arrays very fast

**Benchmark:** ~500ns average for 100 levels

### 3. Manual Doubly-Linked Lists

**Decision:** Hand-coded lists for orders at each price

**Rationale:**
- Full control over memory layout
- O(1) deletion with pointer
- No external dependencies
- Simple implementation (~30 lines)
- Cache-friendly node packing

### 4. Lock-Free Queues

**Decision:** SPSC queues with cache-line alignment

**Rationale:**
- **Zero lock contention** - Each thread owns one end
- **Cache-line isolation** - Prevents false sharing
- ~100-500ns latency per hop
- Simple atomic operations

**Why not lock-based:**
- Locks add 1-10μs latency
- Potential for priority inversion
- More complex error handling

### 5. TCP Multi-Client with epoll

**Decision:** Event-driven I/O with client isolation

**Rationale:**
- **Scales to 100s of clients** - O(active fds), not O(total fds)
- **Non-blocking** - No thread per client
- **Client isolation** - Orders track ownership
- **Auto-cleanup** - Cancel orders on disconnect

### 6. Fixed Array Price Levels

**Decision:** 10,000 price level slots per side

**Rationale:**
- Typical order books: < 100 active levels
- Avoids dynamic allocation
- Predictable memory usage
- Fast binary search

**Memory:** ~800KB per order book (acceptable)

---

## Performance Characteristics

### Throughput

| Component | Throughput | Notes |
|-----------|-----------|-------|
| UDP Receiver | 1-10M pkts/sec | Network limited |
| TCP Receiver | 100K-1M msgs/sec | System call limited |
| Matching Engine | 1-5M orders/sec | CPU limited |
| Memory Pool Alloc | 100M ops/sec | Just index manipulation |

### Latency

| Operation | Latency | Notes |
|-----------|---------|-------|
| Pool allocation | 5-10 cycles | Index pop/push |
| Lock-free queue hop | 100-500ns | Cache-line aligned atomics |
| CSV parsing | 500-2000ns | String parsing overhead |
| Binary parsing | 50-200ns | Memcpy + ntohl |
| Binary search (100 levels) | 200-500ns | ~7 comparisons |
| Order book lookup | 100-500ns | Hash table + list walk |
| Full matching (no fill) | 500-1000ns | Add to book |
| Full matching (fill) | 1-10μs | Depends on book depth |
| End-to-end (UDP) | 10-50μs | Network + processing |

### Memory Usage

| Component | Memory | Notes |
|-----------|--------|-------|
| Order pool | ~1.5MB | 10K × 150 bytes |
| Hash entry pool | ~800KB | 10K × 80 bytes |
| Input queue | ~2MB | 16K × 128 bytes |
| Output queue | ~2MB | 16K × 128 bytes |
| Order book (empty) | ~800KB | 2 × 10K × 40 bytes |
| Order book (1000 orders) | ~900KB | +100KB for orders |
| **Total (typical)** | **10-20MB** | Predictable, pre-allocated |

---

## Project Structure

```
matching-engine-c/
├── build.sh                         # Build script with test modes
├── CMakeLists.txt                   # CMake configuration
│
├── documentation/                   # All documentation
│   ├── ARCHITECTURE.md             # This file
│   ├── BUILD.md
│   ├── PROTOCOLS.md
│   ├── QUICK_START.md
│   └── TESTING.md
│
├── include/                         # Header files
│   ├── core/                       # Core matching logic
│   │   ├── matching_engine.h
│   │   ├── order_book.h
│   │   └── order.h
│   ├── network/                    # Network I/O
│   │   ├── message_framing.h
│   │   ├── tcp_connection.h
│   │   ├── tcp_listener.h
│   │   └── udp_receiver.h
│   ├── protocol/                   # Message protocols
│   │   ├── binary/                 # Binary protocol
│   │   │   ├── binary_message_formatter.h
│   │   │   ├── binary_message_parser.h
│   │   │   └── binary_protocol.h
│   │   ├── csv/                    # CSV protocol
│   │   │   ├── message_formatter.h
│   │   │   └── message_parser.h
│   │   ├── message_types.h
│   │   └── message_types_extended.h
│   └── threading/                  # Threading components
│       ├── lockfree_queue.h
│       ├── output_publisher.h
│       ├── output_router.h
│       ├── processor.h
│       └── queues.h
│
├── src/                            # Implementation (mirrors include/)
│   ├── core/
│   │   ├── matching_engine.c       # ~400 lines
│   │   └── order_book.c            # ~900 lines
│   ├── network/
│   │   ├── message_framing.c
│   │   ├── tcp_connection.c
│   │   ├── tcp_listener.c
│   │   └── udp_receiver.c
│   ├── protocol/
│   │   ├── binary/
│   │   │   ├── binary_message_formatter.c
│   │   │   └── binary_message_parser.c
│   │   └── csv/
│   │       ├── message_formatter.c
│   │       └── message_parser.c
│   ├── threading/
│   │   ├── lockfree_queue.c
│   │   ├── output_publisher.c
│   │   ├── output_router.c
│   │   ├── processor.c
│   │   └── queues.c
│   └── main.c
│
├── tests/                          # Unity test framework
│   ├── core/
│   │   ├── test_matching_engine.c
│   │   └── test_order_book.c
│   ├── protocol/
│   │   ├── test_message_formatter.c
│   │   └── test_message_parser.c
│   ├── scenarios/
│   │   ├── test_scenarios_even.c
│   │   └── test_scenarios_odd.c
│   ├── test_runner.c
│   ├── unity.c
│   └── unity.h
│
└── tools/                          # Test clients and tools
    ├── binary_client.c
    ├── binary_decoder.c
    └── tcp_client.c
```

**Total:** ~5,500 lines of production code, ~2,500 lines of tests

---

## Future Enhancements

### Potential Improvements

**Performance:**
- SIMD optimization for batch operations
- Kernel-bypass networking (DPDK, io_uring)
- Huge pages for memory pools
- CPU pinning for threads

**Features:**
- Level 2 market data (full book snapshots)
- Order types (stop-loss, iceberg, IOC, FOK)
- Market making support
- Cross orders (internal matching)
- Auction modes (opening/closing)

**Reliability:**
- Persistent order book (crash recovery)
- Message replay from log
- Hot standby failover
- Checkpointing

**Observability:**
- Latency histograms (p50, p99, p99.9)
- Memory pool utilization graphs
- Order book depth metrics
- Per-client statistics

---

## See Also

- [Quick Start Guide](QUICK_START.md) - Get up and running
- [Protocols](PROTOCOLS.md) - Message format specifications
- [Testing](TESTING.md) - Comprehensive testing guide
- [Build Instructions](BUILD.md) - Detailed build guide
- [System Overview](#system-overview)
- [Threading Model](#threading-model)
- [Data Flow](#data-flow)
- [Core Components](#core-components)
- [Data Structures](#data-structures)
- [C Port Details](#c-port-details)
- [Design Decisions](#design-decisions)
- [Performance Characteristics](#performance-characteristics)
- [Project Structure](#project-structure)

---

## System Overview

The Matching Engine is a multi-threaded, lock-free system that processes orders through three primary threads communicating via lock-free queues. It supports both TCP (multi-client) and UDP (high-throughput) modes with automatic protocol detection.

### High-Level Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                        Matching Engine                           │
├─────────────────────────────────────────────────────────────────┤
│                                                                   │
│  ┌──────────────┐   ┌────────────┐   ┌───────────────┐         │
│  │   TCP/UDP    │──▶│  Lock-Free │──▶│   Processor   │         │
│  │   Receiver   │   │   Queue    │   │  (Matching)   │         │
│  │  (Thread 1)  │   │ (16K msgs) │   │  (Thread 2)   │         │
│  └──────────────┘   └────────────┘   └───────────────┘         │
│         │                                      │                 │
│         │ Format Detection                     │                 │
│         │ (CSV/Binary)                         │                 │
│         │                                      ▼                 │
│         │                            ┌────────────────┐          │
│         │                            │   Lock-Free    │          │
│         │                            │  Output Queue  │          │
│         │                            │  (16K msgs)    │          │
│         │                            └────────────────┘          │
│         │                                      │                 │
│         │                                      ▼                 │
│         │                            ┌───────────────┐           │
│         │                            │    Output     │           │
│         │                            │   Publisher   │           │
│         │                            │  (Thread 3)   │           │
│         │                            └───────────────┘           │
│         │                                      │                 │
│         └──────────────────────────────────────┘                 │
│                    (TCP per-client routing)                      │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## Threading Model

### Three-Thread Pipeline

**Thread 1: TCP/UDP Receiver**
- Handles network I/O (epoll for TCP, recvfrom for UDP)
- Detects message format (CSV vs Binary)
- Parses messages into internal format
- Enqueues to input queue
- Lock-free queue write (single producer)

**Thread 2: Processor**
- Dequeues messages in batches (up to 32)
- Routes to appropriate order book (by symbol)
- Executes matching logic (price-time priority)
- Generates output messages
- Enqueues to output queue
- Lock-free queue read/write (single consumer/producer)

**Thread 3: Output Publisher**
- Dequeues output messages
- Formats as CSV or Binary
- Routes to appropriate client(s) in TCP mode
- Publishes to stdout or TCP sockets
- Lock-free queue read (single consumer)

### Lock-Free Communication

```c
// Single Producer, Single Consumer (SPSC) Queue
typedef struct {
    _Alignas(64) atomic_uint_fast64_t read_pos;   // Consumer cache line
    _Alignas(64) atomic_uint_fast64_t write_pos;  // Producer cache line
    _Alignas(64) input_msg_t data[16384];         // Data cache lines
} input_queue_t;
```

**Key Properties:**
- Cache-line aligned (64 bytes) to prevent false sharing
- Atomic operations for synchronization
- No locks or mutexes
- Typical latency: 100-500ns per hop

---

## Data Flow

### TCP Multi-Client Mode

```
Client 1 ─┐
Client 2 ─┼──▶ TCP Listener ──▶ Input Queue ──▶ Processor ──▶ Output Router
Client 3 ─┘    (epoll)          (lock-free)     (matching)    (per-client)
    │                                                │
    └────────────────────────────────────────────────┘
              (routed output messages)
```

### UDP High-Throughput Mode

```
UDP Packets ──▶ UDP Receiver ──▶ Input Queue ──▶ Processor ──▶ Output Publisher
(port 1234)    (recvfrom)       (lock-free)     (matching)        (stdout)
```

### Message Flow Example

```
1. Client sends: "N, 1, IBM, 100, 50, B, 1"
2. Receiver parses → input_msg_t{type=NEW_ORDER, ...}
3. Enqueue to input queue
4. Processor dequeues batch of 32 messages
5. Route to IBM order book
6. Match order → Generate output_msg_t[]
7. Enqueue to output queue (with client routing info)
8. Publisher dequeues and formats
9. Route to client's TCP socket or stdout
```

---

## Core Components

### Message Types (`message_types.h`)

Tagged unions replace C++ `std::variant`:

```c
typedef enum {
    MSG_NEW_ORDER,
    MSG_CANCEL_ORDER,
    MSG_FLUSH
} input_msg_type_t;

typedef struct {
    input_msg_type_t type;
    union {
        struct { uint32_t user_id; char symbol[16]; ... } new_order;
        struct { uint32_t user_id; uint32_t user_order_id; } cancel;
        // flush has no data
    } data;
} input_msg_t;
```

### Order Book (`order_book.h`)

**Price Levels:**
```c
typedef struct {
    uint32_t price;
    order_t* head;  // Doubly-linked list of orders
    order_t* tail;
    uint32_t total_quantity;
} price_level_t;

typedef struct {
    price_level_t buy_levels[10000];   // Fixed array
    price_level_t sell_levels[10000];
    int num_buy_levels;
    int num_sell_levels;
    // Hash table for O(1) order lookup
    order_hash_table_t order_lookup;
} order_book_t;
```

**Matching Algorithm:**
1. Binary search for price level
2. Time priority within price level (FIFO)
3. Partial fills supported
4. Generate trade messages
5. Update top-of-book

### Matching Engine (`matching_engine.h`)

Multi-symbol orchestrator:

```c
typedef struct {
    order_book_t order_books[MAX_SYMBOLS];  // Fixed array of books
    char symbols[MAX_SYMBOLS][16];
    int num_symbols;
} matching_engine_t;
```

Routes messages to appropriate order book by symbol.

### Lock-Free Queue (`lockfree_queue.h`)

SPSC queue implemented as C macro "template":

```c
#define DECLARE_LOCKFREE_QUEUE(type, name) \
    typedef struct { \
        _Alignas(64) atomic_uint_fast64_t read_pos; \
        _Alignas(64) atomic_uint_fast64_t write_pos; \
        _Alignas(64) type data[QUEUE_CAPACITY]; \
    } name##_t;
```

Usage:
```c
DECLARE_LOCKFREE_QUEUE(input_msg_t, input_queue)
DECLARE_LOCKFREE_QUEUE(output_msg_t, output_queue)
```

---

## Data Structures

### Order Structure

```c
typedef struct order {
    uint32_t user_id;
    uint32_t user_order_id;
    uint32_t price;
    uint32_t quantity;
    char side;  // 'B' or 'S'
    uint64_t timestamp_ns;  // Nanosecond precision
    struct order* next;     // Doubly-linked list
    struct order* prev;
} order_t;
```

### Price Level Management

**Binary Search:**
```c
// O(log N) lookup in sorted array
int find_price_level(price_level_t* levels, int num_levels, uint32_t price);
```

**Insertion:**
```c
// Maintain sorted order, shift array if needed
void insert_price_level(order_book_t* book, uint32_t price, char side);
```

### Order Hash Table

```c
typedef struct {
    order_t* buckets[ORDER_HASH_SIZE];  // 10007 buckets (prime)
} order_hash_table_t;

// O(1) average case lookup for cancellation
order_t* hash_table_find(order_hash_table_t* table, uint32_t user_order_id);
```

---

## C Port Details

### How C++ Features Were Replaced

| C++ Feature | C Replacement | Implementation |
|-------------|---------------|----------------|
| `std::variant<A,B,C>` | Tagged union | `struct { type_t type; union { A a; B b; C c; } data; }` |
| `std::optional<T>` | Bool + out param | `bool func(..., T* out)` |
| `std::string` | Fixed char array | `char symbol[16]` with bounds checking |
| `std::map<K,V>` | Sorted array | Binary search on `price_level_t levels[10000]` |
| `std::list<T>` | Manual list | `struct node { T data; node *next, *prev; }` |
| `std::unordered_map<K,V>` | Hash table | Custom chaining implementation |
| `std::vector<T>` | Fixed array | `T arr[MAX]; int count;` |
| `std::thread` | pthreads | `pthread_create()`, `pthread_join()` |
| `std::atomic<T>` | C11 atomics | `_Atomic(T)` or `atomic_uint_fast64_t` |
| `std::chrono` | POSIX time | `clock_gettime(CLOCK_REALTIME)` |
| Templates | Macros | `#define DECLARE_TYPE(T) ...` |
| Namespaces | Prefixes | `order_book_create()`, `order_book_add()` |
| Destructors | Explicit cleanup | `order_book_destroy(book)` |

### Memory Management

**No STL containers means:**
- Manual memory management (malloc/free)
- Explicit cleanup functions
- More control over allocation patterns
- Potential for memory leaks if not careful

**Best practices:**
```c
// Create
order_book_t* book = order_book_create("IBM");

// Use
order_book_add_order(book, ...);

// Cleanup
order_book_destroy(book);
```

---

## Design Decisions

### 1. Fixed Arrays vs Dynamic Allocation

**Decision:** Fixed-size arrays for price levels (10,000 slots)

**Rationale:**
- Predictable memory usage (~800KB per order book)
- Better cache locality (contiguous memory)
- No malloc/free in matching hot path
- Typical order books have < 100 active price levels
- Avoids fragmentation

**Trade-off:** Maximum 10,000 distinct price levels per symbol

### 2. Binary Search vs Hash Table for Prices

**Decision:** Binary search on sorted array

**Rationale:**
- O(log N) is acceptable for N < 10,000
- Maintains natural price ordering
- Better cache performance than trees
- Simpler than red-black trees
- Modern CPUs make small arrays very fast

**Benchmark:** ~500ns average for 100 levels

### 3. Manual Doubly-Linked Lists

**Decision:** Hand-coded doubly-linked lists for orders at each price

**Rationale:**
- Full control over memory layout
- O(1) deletion with pointer
- No external dependencies
- Simple implementation (~50 lines)
- Cache-friendly node packing

### 4. Lock-Free Queues

**Decision:** Single Producer Single Consumer (SPSC) queues

**Rationale:**
- Zero lock contention
- Cache-line alignment prevents false sharing
- ~100-500ns latency per hop
- Simple implementation (atomic read/write pointers)

**Why not lock-based:**
- Locks add ~1-10μs latency
- Potential for priority inversion
- More complex error handling

### 5. Dual Protocol Support

**Decision:** Auto-detection via magic byte (0x4D)

**Rationale:**
- Zero configuration required
- Backward compatible with CSV tests
- Performance boost for binary clients (5-10x)
- Mixed-mode operation for gradual migration

### 6. Macro "Templates" for Queues

**Decision:** C macros instead of void* or code generation

**Rationale:**
- Type safety at compile time
- Zero runtime overhead
- Similar to C++ templates
- Explicit code generation (visible in preprocessed output)

**Example:**
```c
DECLARE_LOCKFREE_QUEUE(input_msg_t, input_queue)
// Expands to full input_queue_t struct definition
```

### 7. TCP Multi-Client Architecture

**Decision:** epoll-based event loop with per-client isolation

**Rationale:**
- Scales to hundreds of clients
- Non-blocking I/O
- Per-client order ownership validation
- Automatic cleanup on disconnect

---

## Performance Characteristics

### Throughput

| Component | Throughput | Bottleneck |
|-----------|-----------|------------|
| UDP Receiver | 1-10M packets/sec | Network bandwidth |
| TCP Receiver | 100K-1M messages/sec | System calls (epoll) |
| Processor | 1-5M orders/sec | Matching logic |
| Output Publisher | 500K-1M messages/sec | stdout/TCP write |

### Latency

| Operation | Latency | Notes |
|-----------|---------|-------|
| Lock-free queue hop | 100-500ns | Cache-line aligned atomics |
| CSV parsing | 500-2000ns | String parsing overhead |
| Binary parsing | 50-200ns | Memcpy + ntohl |
| Order book lookup | 100-500ns | Binary search + hash |
| Matching (no fill) | 500-1000ns | Add to order book |
| Matching (full fill) | 1-10μs | Depends on book depth |
| End-to-end (UDP) | 10-50μs | Network + processing |

### Memory Usage

| Component | Memory | Notes |
|-----------|--------|-------|
| Input queue | ~2MB | 16,384 × ~128 bytes |
| Output queue | ~2MB | 16,384 × ~128 bytes |
| Order book (empty) | ~800KB | 2 × 10,000 × 40 bytes |
| Order book (1000 orders) | ~900KB | +100KB for orders |
| Total (10 symbols) | 10-50MB | Typical usage |

### Protocol Comparison

| Metric | CSV | Binary | Improvement |
|--------|-----|--------|-------------|
| New Order Size | 40-50 bytes | 30 bytes | 40-60% smaller |
| Cancel Size | 20-30 bytes | 11 bytes | 60-70% smaller |
| Parsing Time | 500-2000ns | 50-200ns | 5-10x faster |
| Network Bandwidth | Baseline | 50-70% of CSV | 2x more efficient |
| Human Readable | ✓ | ✗ | Trade-off |

### Scalability

**Vertical (single machine):**
- CPU-bound at ~5M orders/sec
- Memory: ~50MB per 10K active orders
- Network: ~1-2Gbps saturated

**Horizontal (theoretical):**
- Shard by symbol (independent order books)
- Replicate for high availability
- Aggregate market data

---

## Project Structure

```
matching-engine-c/
├── include/                    # Header files
│   ├── message_types.h         # Core message definitions
│   ├── order.h                 # Order structure
│   ├── order_book.h            # Order book (price levels + hash)
│   ├── matching_engine.h       # Multi-symbol orchestrator
│   ├── message_parser.h        # CSV input parser
│   ├── message_formatter.h     # CSV output formatter
│   ├── binary_protocol.h       # Binary message specs
│   ├── binary_message_parser.h # Binary input parser
│   ├── binary_message_formatter.h # Binary output formatter
│   ├── lockfree_queue.h        # SPSC queue macros
│   ├── udp_receiver.h          # UDP receiver thread
│   ├── tcp_listener.h          # TCP multi-client thread
│   ├── processor.h             # Processor thread
│   └── output_publisher.h      # Output thread
│
├── src/                        # Implementation files
│   ├── main.c                  # Entry point (400 lines)
│   ├── order_book.c            # Matching logic (800+ lines)
│   ├── matching_engine.c       # Symbol routing (200 lines)
│   ├── message_parser.c        # CSV parsing (300 lines)
│   ├── message_formatter.c     # CSV formatting (200 lines)
│   ├── binary_message_parser.c # Binary parsing (150 lines)
│   ├── binary_message_formatter.c # Binary formatting (150 lines)
│   ├── udp_receiver.c          # UDP logic (300 lines)
│   ├── tcp_listener.c          # TCP epoll logic (600 lines)
│   ├── processor.c             # Batch processing (200 lines)
│   └── output_publisher.c      # Output routing (250 lines)
│
├── tools/                      # Binary protocol tools
│   ├── binary_client.c         # Binary test client
│   ├── tcp_client.c            # TCP test client
│   └── binary_decoder.c        # Binary→CSV decoder
│
└── tests/                      # Unity test framework
    ├── unity.[ch]              # Unity framework
    ├── test_runner.c           # Test main()
    ├── test_order_book.c       # Core matching tests
    ├── test_message_parser.c   # Parser validation
    └── ...                     # More test files
```

**Total:** ~5,000 lines of production code, ~2,000 lines of tests

---

## Future Enhancements

### Potential Improvements

**Performance:**
- SIMD optimization for batch operations
- Kernel-bypass networking (DPDK, io_uring)
- Lock-free hash table for order lookup
- Memory pool allocator for orders

**Features:**
- Level 2 market data (full book snapshots)
- Order types (stop-loss, iceberg, IOC, FOK)
- Market making support
- Cross orders (internal matching)
- Auction modes (opening/closing)

**Reliability:**
- Persistent order book (crash recovery)
- Message replay from log
- Hot standby failover
- Checkpointing

**Observability:**
- Latency histograms
- Performance counters
- Order book depth metrics
- Client session tracking

---

## See Also

- [Quick Start Guide](QUICK_START.md) - Get up and running
- [Protocols](PROTOCOLS.md) - Message format specifications
- [Testing](TESTING.md) - Comprehensive testing guide
- [Build Instructions](BUILD.md) - Detailed build guide
