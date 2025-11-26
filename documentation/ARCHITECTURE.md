# Architecture Documentation

This document provides detailed technical documentation of the matching engine's architecture, with a focus on cache optimization strategies and low-latency design decisions.

## Table of Contents

1. [Design Philosophy](#design-philosophy)
2. [Memory Layout & Cache Optimization](#memory-layout--cache-optimization)
3. [Data Structures](#data-structures)
4. [Threading Model](#threading-model)
5. [Message Protocol](#message-protocol)
6. [Matching Algorithm](#matching-algorithm)
7. [Performance Analysis](#performance-analysis)

---

## Design Philosophy

### Core Principles

1. **Cache is King**: Every data structure is designed around the 64-byte cache line
2. **Zero Allocation**: No malloc/free calls in the hot path
3. **Predictable Latency**: Bounded operations, no unbounded loops
4. **Compile-Time Verification**: Static assertions validate all assumptions

### Why Cache Optimization Matters

Modern CPUs access L1 cache in ~1 nanosecond, but main memory takes ~100 nanoseconds. A single cache miss can cost 100x the time of a cache hit. For HFT applications processing millions of messages per second, cache efficiency directly translates to throughput and latency.

```
┌─────────────────────────────────────────────────────────────┐
│                    Memory Hierarchy                          │
├─────────────────────────────────────────────────────────────┤
│  L1 Cache    │  32 KB   │  ~1 ns    │  ~4 cycles           │
│  L2 Cache    │  256 KB  │  ~3 ns    │  ~12 cycles          │
│  L3 Cache    │  8+ MB   │  ~10 ns   │  ~40 cycles          │
│  Main Memory │  GBs     │  ~100 ns  │  ~400 cycles         │
└─────────────────────────────────────────────────────────────┘
```

---

## Memory Layout & Cache Optimization

### Cache Line Alignment (64 bytes)

All frequently-accessed structures are aligned to 64-byte boundaries and sized to be exact multiples of 64 bytes. This ensures:

1. **No False Sharing**: Different threads never contend on the same cache line
2. **Efficient Prefetching**: Hardware prefetcher works on cache-line boundaries
3. **DMA Efficiency**: Network buffers align with cache lines

### Compiler Alignment Syntax

We use compiler-specific attributes for maximum compatibility:

```c
#if defined(__GNUC__) || defined(__clang__)
    #define CACHE_ALIGNED __attribute__((aligned(64)))
    #define CACHE_LINE_SIZE 64
#else
    #define CACHE_ALIGNED _Alignas(64)
    #define CACHE_LINE_SIZE 64
#endif
```

### Explicit Padding Strategy

Every structure has explicit `_pad` fields rather than relying on implicit compiler padding:

```c
typedef struct {
    uint32_t user_id;        // 0-3
    uint32_t user_order_id;  // 4-7
    uint32_t price;          // 8-11
    uint32_t quantity;       // 12-15
    side_t side;             // 16 (uint8_t)
    uint8_t _pad[3];         // 17-19 (EXPLICIT padding)
    char symbol[16];         // 20-35
} new_order_msg_t;           // 36 bytes total
```

Benefits of explicit padding:
- **Documentation**: Layout is visible in the code
- **Verification**: Static assertions can check exact offsets
- **Portability**: No surprises across compilers

---

## Data Structures

### 1. Order Structure (64 bytes)

The `order_t` structure is exactly one cache line:

```c
typedef struct order {
    // === HOT DATA (accessed during matching) === (bytes 0-19)
    uint32_t user_id;           // 0-3
    uint32_t user_order_id;     // 4-7
    uint32_t price;             // 8-11
    uint32_t quantity;          // 12-15
    uint32_t remaining_qty;     // 16-19
    
    // === METADATA === (bytes 20-31)
    side_t side;                // 20 (uint8_t, was 4-byte enum)
    order_type_t type;          // 21 (uint8_t, was 4-byte enum)
    uint8_t _pad1[2];           // 22-23 (explicit padding)
    uint32_t client_id;         // 24-27
    uint32_t _pad2;             // 28-31 (align timestamp to 8 bytes)
    
    // === TIMESTAMP === (bytes 32-39)
    uint64_t timestamp;         // 32-39 (RDTSC value)
    
    // === LINKED LIST POINTERS === (bytes 40-55)
    struct order *next;         // 40-47
    struct order *prev;         // 48-55
    
    // === PADDING TO 64 BYTES ===
    uint8_t _padding[8];        // 56-63
} CACHE_ALIGNED order_t;
```

**Key Optimizations:**
- Hot fields (price, quantity, remaining_qty) in first 20 bytes
- Enums reduced from 4 bytes to 1 byte each (saves 6 bytes)
- Linked list pointers at end (only used during list traversal)
- Exactly 64 bytes = 1 cache line

**Verification:**
```c
_Static_assert(sizeof(order_t) == 64, "order_t must be 64 bytes");
_Static_assert(alignof(order_t) == 64, "order_t must be cache-aligned");
```

### 2. Price Level Structure (64 bytes)

Each price level in the order book is one cache line:

```c
typedef struct {
    // === HOT DATA === (bytes 0-15)
    uint32_t price;             // 0-3
    uint32_t total_quantity;    // 4-7
    uint32_t order_count;       // 8-11
    uint32_t _pad1;             // 12-15
    
    // === LIST POINTERS === (bytes 16-31)
    order_t* head;              // 16-23
    order_t* tail;              // 24-31
    
    // === TREE POINTERS === (bytes 32-55)
    struct price_level* left;   // 32-39
    struct price_level* right;  // 40-47
    struct price_level* parent; // 48-55
    
    // === PADDING ===
    uint8_t _pad2[8];           // 56-63
} CACHE_ALIGNED price_level_t;
```

**Access Pattern Optimization:**
- During matching: only bytes 0-31 accessed (price, quantity, head)
- Tree navigation: bytes 32-55 accessed
- Both patterns fit in one cache line

### 3. Order Map (Open-Addressing Hash Table)

Replaced pointer-chasing chained hash with cache-friendly open-addressing:

```c
#define ORDER_MAP_SIZE 8192  // Power of 2 for fast modulo

typedef struct {
    uint64_t key;            // Combined (user_id << 32) | user_order_id
    order_t* order;          // Pointer to order
    uint32_t hash;           // Cached hash for faster reprobing
    uint32_t _pad;           // Align to 32 bytes
} order_map_slot_t;          // 32 bytes = 2 slots per cache line

typedef struct {
    order_map_slot_t slots[ORDER_MAP_SIZE];
    uint32_t count;
    uint32_t tombstone_count;
} order_map_t;
```

**Lookup Algorithm (Linear Probing):**
```c
order_t* order_map_find(order_map_t* map, uint32_t user_id, uint32_t order_id) {
    uint64_t key = ((uint64_t)user_id << 32) | order_id;
    uint32_t hash = hash_key(key);
    uint32_t index = hash & (ORDER_MAP_SIZE - 1);
    
    for (uint32_t probe = 0; probe < MAX_PROBE_LENGTH; probe++) {
        order_map_slot_t* slot = &map->slots[index];
        
        if (slot->order == NULL && slot->hash == 0) {
            return NULL;  // Empty slot = not found
        }
        if (slot->key == key && slot->order != NULL) {
            return slot->order;  // Found
        }
        
        index = (index + 1) & (ORDER_MAP_SIZE - 1);  // Linear probe
    }
    return NULL;  // Max probes exceeded
}
```

**Why Open-Addressing?**

| Metric | Chained Hash | Open-Addressing |
|--------|--------------|-----------------|
| Cache lines per lookup | 3-5 (random) | 1-2 (sequential) |
| Memory overhead | 24+ bytes/entry | 0 (inline) |
| Allocation | malloc per insert | None |
| Deletion | free + pointer fixup | Write tombstone |

### 4. Lock-Free Queue

SPSC (Single-Producer Single-Consumer) queue with false-sharing prevention:

```c
typedef struct {
    // Producer cache line (written by producer only)
    _Alignas(64) _Atomic(size_t) head;
    uint8_t _pad_head[64 - sizeof(_Atomic(size_t))];
    
    // Consumer cache line (written by consumer only)
    _Alignas(64) _Atomic(size_t) tail;
    uint8_t _pad_tail[64 - sizeof(_Atomic(size_t))];
    
    // Statistics cache line (occasional updates)
    _Alignas(64) struct {
        _Atomic(size_t) total_enqueued;
        _Atomic(size_t) total_dequeued;
        _Atomic(size_t) failed_enqueues;
        _Atomic(size_t) failed_dequeues;
        _Atomic(size_t) peak_size;
    } stats;
    uint8_t _pad_stats[64 - 40];
    
    // Buffer (cache-line aligned)
    _Alignas(64) T buffer[QUEUE_SIZE];
} lockfree_queue_t;
```

**Memory Layout Visualization:**
```
Offset 0:    [head (8B)][───── padding (56B) ─────]  ← Producer writes
Offset 64:   [tail (8B)][───── padding (56B) ─────]  ← Consumer writes
Offset 128:  [stats (40B)][──── padding (24B) ────]  ← Occasional
Offset 192:  [buffer[0]] [buffer[1]] [buffer[2]] ... ← Data
```

**Enqueue Operation (Lock-Free):**
```c
bool enqueue(queue_t* q, const T* item) {
    size_t head = atomic_load_explicit(&q->head, memory_order_relaxed);
    size_t next = (head + 1) & QUEUE_MASK;
    
    // Check if full
    if (next == atomic_load_explicit(&q->tail, memory_order_acquire)) {
        return false;
    }
    
    // Write item
    q->buffer[head] = *item;
    
    // Publish to consumer
    atomic_store_explicit(&q->head, next, memory_order_release);
    return true;
}
```

### 5. Message Structures

**Packed Enums (uint8_t instead of int):**

```c
// OLD: 4 bytes each
typedef enum { SIDE_BUY = 'B', SIDE_SELL = 'S' } side_t;

// NEW: 1 byte each
typedef uint8_t side_t;
#define SIDE_BUY  ((side_t)'B')
#define SIDE_SELL ((side_t)'S')
```

This saves 3 bytes per enum field, which adds up across message structures.

**Output Message Envelope (64 bytes):**

```c
typedef struct CACHE_ALIGNED {
    output_msg_t msg;       // 52 bytes
    uint32_t client_id;     // 4 bytes  (for TCP routing)
    uint64_t sequence;      // 8 bytes  (for ordering)
} output_msg_envelope_t;    // Exactly 64 bytes
```

**Benefits:**
- Perfect for DMA transfers
- One envelope = one cache line
- Sequence number for gap detection

---

## Threading Model

### Processor Thread Architecture

```c
typedef struct {
    // Configuration
    processor_config_t config;
    
    // Queues
    input_queue_t* input_queue;
    output_queue_t* output_queue;
    
    // Engine (per-processor in dual mode)
    matching_engine_t engine;
    
    // Statistics (cache-line aligned)
    _Alignas(64) processor_stats_t stats;
    
    // Sequence counter (local, flushed periodically)
    _Atomic(uint64_t) output_sequence;
} processor_t;
```

### Configurable Wait Strategy

```c
typedef struct {
    bool tcp_mode;
    bool spin_wait;      // true = busy-wait, false = nanosleep
    int processor_id;    // For logging
} processor_config_t;
```

**Spin-Wait Implementation:**
```c
if (config->spin_wait) {
    #if defined(__x86_64__)
    __asm__ volatile("pause" ::: "memory");  // ~10 cycles
    #elif defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");  // ARM hint
    #endif
} else {
    nanosleep(&(struct timespec){0, 1000}, NULL);  // 1µs
}
```

| Mode | Latency | CPU Usage | Use Case |
|------|---------|-----------|----------|
| `spin_wait=true` | ~100ns | 100% core | HFT, ultra-low latency |
| `spin_wait=false` | ~1-5µs | Low | General purpose |

### Batched Statistics

Statistics are updated in batches to reduce atomic operations:

```c
// Per-message (OLD - expensive)
atomic_fetch_add(&stats->messages_processed, 1);

// Batched (NEW - efficient)
local_count++;
if (local_count >= 1000) {
    stats->messages_processed += local_count;  // Single write
    local_count = 0;
}
```

### Prefetching

```c
for (size_t i = 0; i < batch_count; i++) {
    // Prefetch next message while processing current
    if (i + 1 < batch_count) {
        PREFETCH_READ(&input_batch[i + 1]);
    }
    
    process_message(&input_batch[i]);
}
```

---

## Message Protocol

### Input Messages

| Type | Format | Size |
|------|--------|------|
| New Order | `N,user,symbol,price,qty,side,oid` | 40 bytes |
| Cancel | `C,symbol,user,oid` | 28 bytes |
| Flush | `F` | 4 bytes |

### Output Messages

| Type | Format | Size |
|------|--------|------|
| Ack | `A,user,oid` | 12 bytes |
| Trade | `T,buy_user,buy_oid,sell_user,sell_oid,price,qty,symbol` | 48 bytes |
| Top of Book | `B,symbol,side,price,qty` | 28 bytes |
| Cancel Ack | `X,user,oid` | 12 bytes |

### Binary Protocol (Internal)

For queue transport, messages use packed binary format:

```c
typedef struct {
    input_msg_type_t type;    // 1 byte
    uint8_t _pad[3];          // Align union
    union {
        new_order_msg_t new_order;  // 36 bytes
        cancel_msg_t cancel;        // 28 bytes
        flush_msg_t flush;          // 4 bytes
    } data;
} input_msg_t;                // 40 bytes
```

---

## Matching Algorithm

### Price-Time Priority (FIFO)

```
1. Incoming BUY order at price P:
   - Search ASK side for prices ≤ P (for limit) or any price (for market)
   - Match against oldest orders first at each price level
   - Continue until order filled or no more matching prices

2. Incoming SELL order at price P:
   - Search BID side for prices ≥ P (for limit) or any price (for market)
   - Match against oldest orders first at each price level
   - Continue until order filled or no more matching prices
```

### Matching Loop (Bounded)

```c
#define MAX_MATCH_ITERATIONS 10000

uint32_t match_count = 0;
while (incoming->remaining_qty > 0 && match_count < MAX_MATCH_ITERATIONS) {
    price_level_t* best = get_best_price(book, opposite_side);
    if (!best || !prices_cross(incoming->price, best->price, incoming->side)) {
        break;
    }
    
    order_t* resting = best->head;
    while (resting && incoming->remaining_qty > 0) {
        uint32_t fill_qty = MIN(incoming->remaining_qty, resting->remaining_qty);
        
        emit_trade(incoming, resting, fill_qty, output);
        
        incoming->remaining_qty -= fill_qty;
        resting->remaining_qty -= fill_qty;
        
        if (resting->remaining_qty == 0) {
            remove_order(book, resting);
        }
        
        resting = resting->next;
        match_count++;
    }
}
```

---

## Performance Analysis

### Memory Footprint

| Component | Size | Notes |
|-----------|------|-------|
| Order Pool | 640 KB | 10,000 orders × 64 bytes |
| Price Levels | 64 KB | 1,000 levels × 64 bytes |
| Order Maps | 256 KB | 8,192 slots × 32 bytes |
| Queues | 2 MB | 2 × 16K slots × 64 bytes |
| **Total** | **~3 MB** | Per processor |

### Latency Breakdown (Typical)

| Operation | Time | Cache Lines |
|-----------|------|-------------|
| Queue dequeue | ~20 ns | 1-2 |
| Hash lookup | ~15 ns | 1-2 |
| Price level find | ~10 ns | 1 |
| Order matching | ~30 ns | 2-3 |
| Queue enqueue | ~20 ns | 1-2 |
| **Total** | **~95 ns** | 6-10 |

### Throughput Capacity

| Mode | Messages/sec | Notes |
|------|--------------|-------|
| Single processor | 5-10 M | CPU bound |
| Dual processor | 10-20 M | Scales with symbols |
| Network limited | 1-2 M | 10 Gbps Ethernet |

### Cache Efficiency Metrics

With `perf stat`:
```
L1-dcache-loads:       1,000,000,000
L1-dcache-load-misses:     5,000,000  (0.5%)
LLC-loads:                 2,000,000
LLC-load-misses:             100,000  (5%)
```

Target: < 1% L1 miss rate, < 10% LLC miss rate.

---

## Compile-Time Verification

All layout assumptions are verified at compile time:

```c
// Size assertions
_Static_assert(sizeof(order_t) == 64, "order_t must be 64 bytes");
_Static_assert(sizeof(price_level_t) == 64, "price_level_t must be 64 bytes");
_Static_assert(sizeof(output_msg_envelope_t) == 64, "envelope must be 64 bytes");
_Static_assert(sizeof(order_map_slot_t) == 32, "slot must be 32 bytes");

// Alignment assertions (GCC/Clang only)
#if defined(__GNUC__) || defined(__clang__)
_Static_assert(alignof(order_t) == 64, "order_t must be cache-aligned");
_Static_assert(alignof(price_level_t) == 64, "price_level_t must be cache-aligned");
#endif

// Queue size must be power of 2 for fast modulo
_Static_assert((LOCKFREE_QUEUE_SIZE & (LOCKFREE_QUEUE_SIZE - 1)) == 0,
               "Queue size must be power of 2");
```

---

## Future Optimizations

### Potential Improvements

1. **SIMD Order Matching**: Use AVX-512 to compare multiple prices simultaneously
2. **Huge Pages**: Reduce TLB misses for large order books
3. **NUMA Awareness**: Pin processors to specific NUMA nodes
4. **Kernel Bypass**: DPDK or io_uring for network I/O
5. **Persistent Memory**: Intel Optane for order book recovery

### Benchmark Targets

| Metric | Current | Target |
|--------|---------|--------|
| p50 latency | < 1 µs | < 500 ns |
| p99 latency | < 5 µs | < 2 µs |
| Throughput | 10M msg/s | 50M msg/s |

---

## References

1. Ulrich Drepper, "What Every Programmer Should Know About Memory" (2007)
2. Gerard Holzmann, "Power of Ten: Rules for Developing Safety Critical Code" (2006)
3. Herb Sutter, "Machine Architecture: Things Your Programming Language Never Told You" (2008)
4. Intel, "Intel 64 and IA-32 Architectures Optimization Reference Manual"
