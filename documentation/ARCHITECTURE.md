# Architecture

Detailed system design and implementation of the Matching Engine.

## Table of Contents
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
