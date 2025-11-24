# Architecture

Comprehensive system design of the Matching Engine, emphasizing **zero-allocation memory pools**, lock-free threading, **envelope-based message routing**, and production-grade architecture.

## Table of Contents
- [System Overview](#system-overview)
- [Memory Pool System](#memory-pool-system)
- [Threading Model](#threading-model)
- [Envelope Pattern](#envelope-pattern)
- [Data Flow](#data-flow)
- [Core Components](#core-components)
- [Data Structures](#data-structures)
- [TCP Multi-Client Architecture](#tcp-multi-client-architecture)
- [Message Framing (TCP)](#message-framing-tcp)
- [C Port Details](#c-port-details)
- [Design Decisions](#design-decisions)
- [Performance Characteristics](#performance-characteristics)
- [Project Structure](#project-structure)

---

## System Overview

The Matching Engine is a **production-grade** order matching system built in pure C11. The defining characteristics are:

1. **Zero-allocation hot path** - All memory pre-allocated in pools
2. **Envelope-based routing** - Messages wrapped with client metadata for multi-client support
3. **Lock-free communication** - SPSC queues between threads
4. **Client isolation** - TCP clients validated and isolated

### High-Level Architecture

```
┌──────────────────────────────────────────────────────────────────────────────┐
│                           Matching Engine                                     │
│                     Zero-Allocation Memory Pools                              │
├──────────────────────────────────────────────────────────────────────────────┤
│                                                                               │
│  TCP MODE:                                                                    │
│  ┌──────────────┐    ┌────────────────┐    ┌───────────┐    ┌─────────────┐ │
│  │ TCP Listener │───▶│ Input Envelope │───▶│ Processor │───▶│   Output    │ │
│  │  (Thread 1)  │    │     Queue      │    │ (Thread 2)│    │  Envelope   │ │
│  │              │    │                │    │           │    │   Queue     │ │
│  │ epoll/kqueue │    │ [client_id,    │    │  Unwrap   │    │             │ │
│  │ event loop   │    │  sequence,     │    │  Route    │    │ [client_id, │ │
│  │              │    │  message]      │    │  Match    │    │  sequence,  │ │
│  └──────────────┘    └────────────────┘    └───────────┘    │  message]   │ │
│         │                                                    └──────┬──────┘ │
│         │                                                           │        │
│         │                                                           ▼        │
│         │                                                   ┌─────────────┐  │
│         │                                                   │   Output    │  │
│         │                                                   │   Router    │  │
│         │                                                   │ (Thread 3)  │  │
│         │                                                   └──────┬──────┘  │
│         │                                                          │         │
│         │         ┌────────────────────────────────────────────────┤         │
│         │         │                    │                    │      │         │
│         │         ▼                    ▼                    ▼      │         │
│         │  ┌────────────┐      ┌────────────┐      ┌────────────┐  │         │
│         │  │ Client 1 Q │      │ Client 2 Q │      │ Client N Q │  │         │
│         │  └─────┬──────┘      └─────┬──────┘      └─────┬──────┘  │         │
│         │        │                   │                   │         │         │
│         └────────┴───────────────────┴───────────────────┘         │         │
│                  TCP Listener writes to client sockets             │         │
│                                                                    │         │
│  UDP MODE:                                                         │         │
│  ┌──────────────┐    ┌────────────────┐    ┌───────────┐    ┌─────────────┐ │
│  │ UDP Receiver │───▶│ Input Envelope │───▶│ Processor │───▶│   Output    │ │
│  │  (Thread 1)  │    │     Queue      │    │ (Thread 2)│    │  Publisher  │ │
│  │              │    │                │    │           │    │ (Thread 3)  │ │
│  │ client_id=0  │    │ [0, seq, msg]  │    │           │    │    stdout   │ │
│  └──────────────┘    └────────────────┘    └───────────┘    └─────────────┘ │
│                                                                               │
└──────────────────────────────────────────────────────────────────────────────┘
```

**Key Design Principles:**
- **Zero malloc/free in hot path** - All memory pre-allocated in pools
- **Envelope-based routing** - Messages carry routing metadata
- **Lock-free communication** - SPSC queues between threads
- **Bounded loops** - Every loop has explicit iteration limits
- **Defensive programming** - Parameter validation, bounds checking
- **Client isolation** - TCP clients validated and can't spoof each other

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
memory_pools_t* pools = malloc(sizeof(memory_pools_t));
memory_pools_init(pools);

matching_engine_t engine;
matching_engine_init(&engine, pools);  // Share pools
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
memory_pools_get_stats(pools, &stats);
```

**Typical Usage**:
- Peak usage: 500-2000 orders
- Memory: ~10-20MB pre-allocated
- Failures: 0 (unless pool exhausted)

---

## Threading Model

### TCP Mode: Three-Thread Pipeline

```
┌──────────────┐    ┌───────────────────┐    ┌───────────┐    ┌────────────────────┐    ┌───────────────┐
│ TCP Listener │───▶│ input_envelope_q  │───▶│ Processor │───▶│ output_envelope_q  │───▶│ Output Router │
│  (Thread 1)  │    │     (16K msgs)    │    │ (Thread 2)│    │     (16K msgs)     │    │  (Thread 3)   │
└──────────────┘    └───────────────────┘    └───────────┘    └────────────────────┘    └───────┬───────┘
                                                                                                │
                                                              ┌─────────────────────────────────┼─────────────────┐
                                                              ▼                                 ▼                 ▼
                                                     [client_1.output_q]              [client_2.output_q]    [client_n...]
                                                              │                                 │
                                                              ▼                                 ▼
                                                     TCP Listener writes             TCP Listener writes
```

**Thread 1: TCP Listener**
- epoll (Linux) or kqueue (macOS) event loop
- Accepts new connections, assigns client_id
- Reads framed messages, parses CSV/Binary
- Creates input envelopes with client_id
- Writes output messages from per-client queues

**Thread 2: Processor**
- Dequeues input envelopes in batches (up to 32)
- Extracts client_id for routing
- Routes to appropriate order book (by symbol)
- Allocates from memory pools (NO malloc!)
- Creates output envelopes with routing info
- Trades get TWO envelopes (buyer + seller)

**Thread 3: Output Router**
- Dequeues output envelopes
- Routes to per-client output queues based on client_id
- Drops messages for disconnected clients

### UDP Mode: Three-Thread Pipeline

```
┌──────────────┐    ┌───────────────────┐    ┌───────────┐    ┌────────────────────┐    ┌──────────────────┐
│ UDP Receiver │───▶│ input_envelope_q  │───▶│ Processor │───▶│ output_envelope_q  │───▶│ Output Publisher │
│  (Thread 1)  │    │     (16K msgs)    │    │ (Thread 2)│    │     (16K msgs)     │    │   (Thread 3)     │
└──────────────┘    └───────────────────┘    └───────────┘    └────────────────────┘    └──────────────────┘
   client_id=0                                                                                  │
                                                                                                ▼
                                                                                             stdout
```

**Thread 1: UDP Receiver**
- recvfrom() on UDP socket
- Parses CSV/Binary (auto-detect)
- Creates envelopes with client_id=0

**Thread 2: Processor**
- Same as TCP mode
- client_id=0 for all messages

**Thread 3: Output Publisher**
- Dequeues output envelopes
- Formats as CSV or Binary
- Writes to stdout

### Lock-Free Communication

```c
// Single Producer, Single Consumer (SPSC) Queue
typedef struct {
    _Alignas(64) atomic_size_t head;    // Consumer cache line
    char _pad1[64];                      // Prevent false sharing
    _Alignas(64) atomic_size_t tail;    // Producer cache line
    char _pad2[64];
    input_msg_envelope_t buffer[16384]; // Ring buffer
    size_t capacity;
    size_t index_mask;                  // For fast modulo
} input_envelope_queue_t;
```

**Key Properties:**
- Cache-line aligned (64 bytes) prevents false sharing
- Atomic operations for synchronization (no locks!)
- No contention between threads
- Typical latency: 100-500ns per hop

---

## Envelope Pattern

### What is an Envelope?

An **envelope wraps a message with metadata** needed for routing, tracking, and coordination. The core message (order, trade, etc.) doesn't know about TCP clients - the envelope handles that.

```
┌─────────────────────────────────────┐
│  ENVELOPE                           │
│  ┌─────────────────────────────┐   │
│  │  To: Client #3              │   │  ← Routing metadata
│  │  Sequence: 12345            │   │  ← Tracking metadata
│  └─────────────────────────────┘   │
│                                     │
│  ┌─────────────────────────────┐   │
│  │  ACTUAL MESSAGE             │   │
│  │  (Order, Trade, Ack, etc.)  │   │  ← Business payload
│  └─────────────────────────────┘   │
│                                     │
└─────────────────────────────────────┘
```

### Envelope Structures

```c
/* Input envelope - wraps orders/cancels with sender info */
typedef struct {
    input_msg_t msg;           // The actual order/cancel/flush message
    uint32_t client_id;        // Which client sent this (1-based, 0=UDP)
    uint64_t sequence;         // Sequence number for debugging/ordering
} input_msg_envelope_t;

/* Output envelope - wraps responses with routing info */
typedef struct {
    output_msg_t msg;          // The actual ack/trade/TOB message
    uint32_t client_id;        // Which client should receive this
    uint64_t sequence;         // Sequence number
} output_msg_envelope_t;
```

### Why Envelopes?

#### 1. Separation of Concerns

| Concern | Where It Lives |
|---------|---------------|
| What the message IS | `input_msg_t` / `output_msg_t` |
| Who SENT it | `envelope.client_id` |
| Who RECEIVES it | `envelope.client_id` |
| Message ORDERING | `envelope.sequence` |

The matching engine only cares about **WHAT** - it doesn't need to know about TCP clients.

#### 2. Single Queue Scales to Many Clients

**Without envelopes (bad):**
```c
// One queue per client - O(n) polling!
input_queue_t client_queues[MAX_CLIENTS];

for (int i = 0; i < MAX_CLIENTS; i++) {
    if (!input_queue_empty(&client_queues[i])) {
        // process...
    }
}
```

**With envelopes (good):**
```c
// One queue for all - O(1) dequeue!
input_envelope_queue_t single_queue;

input_msg_envelope_t envelope;
if (input_envelope_queue_dequeue(&single_queue, &envelope)) {
    // Client ID travels WITH the message
    process(envelope.client_id, &envelope.msg);
}
```

#### 3. Enables Per-Client Routing

Trades must be sent to BOTH buyer and seller:

```c
// In processor.c
if (out_msg->type == OUTPUT_MSG_TRADE) {
    uint32_t buy_client = out_msg->data.trade.buy_client_id;
    uint32_t sell_client = out_msg->data.trade.sell_client_id;
    
    // Send to buyer
    output_msg_envelope_t env1 = create_output_envelope(out_msg, buy_client, seq);
    output_envelope_queue_enqueue(&output_queue, &env1);
    
    // Send to seller (if different client)
    if (buy_client != sell_client) {
        output_msg_envelope_t env2 = create_output_envelope(out_msg, sell_client, seq);
        output_envelope_queue_enqueue(&output_queue, &env2);
    }
}
```

#### 4. Sequence Numbers for Debugging

```c
envelope.sequence = 12345;

// In logs:
// [Processor] Processing message seq=12345 from client=3
// [Router] Routed trade seq=12346 to clients 1,3
// [Router] Client 3 disconnected, dropped message seq=12347
```

### Helper Functions

```c
/* Create input envelope from parsed message */
static inline input_msg_envelope_t
create_input_envelope(const input_msg_t* msg,
                      uint32_t client_id,
                      uint64_t sequence) {
    input_msg_envelope_t envelope;
    envelope.msg = *msg;
    envelope.client_id = client_id;
    envelope.sequence = sequence;
    return envelope;
}

/* Create output envelope */
static inline output_msg_envelope_t
create_output_envelope(const output_msg_t* msg,
                       uint32_t client_id,
                       uint64_t sequence) {
    output_msg_envelope_t envelope;
    envelope.msg = *msg;
    envelope.client_id = client_id;
    envelope.sequence = sequence;
    return envelope;
}
```

### Queue Types

```c
/* Declare envelope queues using macro templates */
DECLARE_LOCKFREE_QUEUE(input_msg_envelope_t, input_envelope_queue)
DECLARE_LOCKFREE_QUEUE(output_msg_envelope_t, output_envelope_queue)

/* Usage */
input_envelope_queue_t input_queue;
input_envelope_queue_init(&input_queue);

output_envelope_queue_t output_queue;
output_envelope_queue_init(&output_queue);
```

---

## Data Flow

### TCP Mode: End-to-End Flow

```
1. TCP Client connects → assigned client_id=3
2. Client sends framed message: [len][N, 3, IBM, 100, 50, B, 1]
3. TCP Listener reads, parses CSV → input_msg_t
4. Validate: user_id (3) == client_id (3) ✓
5. Create envelope: {msg, client_id=3, seq=1000}
6. Enqueue to input_envelope_queue

7. Processor dequeues envelope
8. Extract client_id=3 for routing
9. Route to IBM order book
10. Allocate order from pool, set order->client_id=3
11. Match order → Generate trade with Client 1
12. Create trade_msg with buy_client_id=1, sell_client_id=3
13. Create TWO output envelopes:
    - {trade_msg, client_id=1, seq=2000}  → for buyer
    - {trade_msg, client_id=3, seq=2000}  → for seller
14. Enqueue both to output_envelope_queue

15. Output Router dequeues envelope for client_id=1
16. Get client 1's per-client queue
17. Enqueue trade_msg (no envelope needed - queue IS routing)

18. Output Router dequeues envelope for client_id=3
19. Get client 3's per-client queue
20. Enqueue trade_msg

21. TCP Listener polls client queues
22. Dequeue from client 1's queue, format, frame, write to socket
23. Dequeue from client 3's queue, format, frame, write to socket
24. Both clients receive trade notification!
```

### Message Routing Rules

| Message Type | Routing | Destination |
|--------------|---------|-------------|
| **Ack** | Unicast | Originating client only |
| **Cancel Ack** | Unicast | Originating client only |
| **Trade** | Multicast | BOTH buyer and seller |
| **Top-of-Book** | Unicast | Originating client only |

### Two-Stage Queuing (TCP Mode)

```
STAGE 1: Envelope Queues (routing info embedded)
┌──────────────────┐         ┌──────────────────┐
│ Input Envelope Q │   →→→   │ Output Envelope Q│
│ [client_id, msg] │         │ [client_id, msg] │
└──────────────────┘         └──────────────────┘

STAGE 2: Per-Client Queues (no envelope needed)
┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐
│ Client 1 Queue  │  │ Client 2 Queue  │  │ Client N Queue  │
│ [msg, msg, ...] │  │ [msg, msg, ...] │  │ [msg, msg, ...] │
└─────────────────┘  └─────────────────┘  └─────────────────┘
```

**Why two stages?**
1. **Envelope queues** - Single queue, envelope carries client_id for routing
2. **Per-client queues** - Once routed, no envelope needed (queue IS the routing)

---

## Core Components

### Order Book (`src/core/order_book.c`)

**Responsibilities:**
- Maintain price levels (buy/sell sides)
- Match incoming orders (price-time priority)
- Track best bid/ask (top-of-book)
- Generate output messages (acks, trades, TOB updates)
- Use memory pools for all allocations
- Track client_id for cancel-on-disconnect

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
2. Set `order->client_id` for tracking
3. Try to match against opposite side
4. If fully matched → free order back to pool
5. If partially matched → add remainder to book
6. Update top-of-book if needed

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

### Processor (`src/threading/processor.c`)

**Responsibilities:**
- Dequeue input envelopes in batches
- Extract client_id for routing
- Call matching engine
- Create output envelopes with routing logic
- Handle client disconnect cancellations

```c
void* processor_thread(void* arg) {
    processor_t* processor = (processor_t*)arg;
    input_msg_envelope_t input_batch[PROCESSOR_BATCH_SIZE];
    output_buffer_t output_buffer;
    
    while (!atomic_load(processor->shutdown_flag)) {
        // Dequeue batch
        size_t count = 0;
        for (size_t i = 0; i < PROCESSOR_BATCH_SIZE; i++) {
            if (input_envelope_queue_dequeue(processor->input_queue, &input_batch[count])) {
                count++;
            } else {
                break;
            }
        }
        
        // Process each message
        for (size_t i = 0; i < count; i++) {
            input_msg_envelope_t* envelope = &input_batch[i];
            uint32_t client_id = envelope->client_id;
            
            output_buffer_init(&output_buffer);
            matching_engine_process_message(processor->engine, &envelope->msg, 
                                           client_id, &output_buffer);
            
            // Create output envelopes with routing logic
            for (int j = 0; j < output_buffer.count; j++) {
                output_msg_t* out_msg = &output_buffer.messages[j];
                
                if (out_msg->type == OUTPUT_MSG_TRADE) {
                    // Trade → route to BOTH buyer and seller
                    // ... (see envelope pattern section)
                } else {
                    // Ack, Cancel, TOB → route to originating client
                    // ...
                }
            }
        }
    }
}
```

### Output Router (`src/threading/output_router.c`)

**Responsibilities:**
- Dequeue output envelopes
- Route to per-client queues based on client_id
- Drop messages for disconnected clients
- Track statistics

```c
void* output_router_thread(void* arg) {
    output_router_context_t* ctx = (output_router_context_t*)arg;
    output_msg_envelope_t batch[BATCH_SIZE];
    
    while (!atomic_load(ctx->shutdown_flag)) {
        // Dequeue batch of output envelopes
        size_t count = 0;
        for (size_t i = 0; i < BATCH_SIZE; i++) {
            if (output_envelope_queue_dequeue(ctx->input_queue, &batch[count])) {
                count++;
            } else {
                break;
            }
        }
        
        // Route each message
        for (size_t i = 0; i < count; i++) {
            output_msg_envelope_t* envelope = &batch[i];
            
            tcp_client_t* client = tcp_client_get(ctx->client_registry, 
                                                   envelope->client_id);
            
            if (client) {
                // Enqueue to client's output queue (no envelope needed)
                if (tcp_client_enqueue_output(client, &envelope->msg)) {
                    ctx->messages_routed++;
                } else {
                    ctx->messages_dropped++;  // Queue full
                }
            } else {
                ctx->messages_dropped++;  // Client disconnected
            }
        }
    }
}
```

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

### Trade Message (with client routing)

```c
typedef struct {
    char symbol[MAX_SYMBOL_LENGTH];
    uint32_t user_id_buy;
    uint32_t user_order_id_buy;
    uint32_t user_id_sell;
    uint32_t user_order_id_sell;
    uint32_t price;
    uint32_t quantity;

    /* TCP routing - which clients to send trade to */
    uint32_t buy_client_id;
    uint32_t sell_client_id;
} trade_msg_t;
```

### Output Buffer

```c
typedef struct {
    output_msg_t messages[MAX_OUTPUT_MESSAGES];
    int count;
} output_buffer_t;

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
#define MAX_TCP_CLIENTS 100

typedef struct {
    /* Network state */
    int socket_fd;                      // Client socket (-1 if inactive)
    uint32_t client_id;                 // Unique ID (1-based)
    struct sockaddr_in addr;            // Client address
    bool active;                        // true if connected
    
    /* Input framing state */
    framing_read_state_t read_state;    // Handles partial reads
    
    /* Output queue (lock-free SPSC) */
    output_queue_t output_queue;        // Producer: output_router
                                        // Consumer: tcp_listener
    
    /* Output framing state */
    framing_write_state_t write_state;  // Handles partial writes
    bool has_pending_write;
    
    /* Statistics */
    time_t connected_at;
    uint64_t messages_received;
    uint64_t messages_sent;
    uint64_t bytes_received;
    uint64_t bytes_sent;
} tcp_client_t;
```

### Client Registry

```c
typedef struct {
    tcp_client_t clients[MAX_TCP_CLIENTS];
    size_t active_count;
    pthread_mutex_t lock;  // Protects add/remove only
} tcp_client_registry_t;

/* API */
bool tcp_client_add(tcp_client_registry_t* registry, int socket_fd,
                    struct sockaddr_in addr, uint32_t* client_id);
void tcp_client_remove(tcp_client_registry_t* registry, uint32_t client_id);
tcp_client_t* tcp_client_get(tcp_client_registry_t* registry, uint32_t client_id);
```

### Security: Anti-Spoofing Validation

The TCP listener validates that `user_id` in messages matches the assigned `client_id`:

```c
// In tcp_listener.c - handle_client_read()
if (parsed) {
    bool valid = true;
    switch (input_msg.type) {
        case INPUT_MSG_NEW_ORDER:
            if (input_msg.data.new_order.user_id != client_id) {
                fprintf(stderr, "[TCP Listener] Client %u tried to spoof userId %u\n",
                        client_id, input_msg.data.new_order.user_id);
                valid = false;
            }
            break;
        case INPUT_MSG_CANCEL:
            if (input_msg.data.cancel.user_id != client_id) {
                fprintf(stderr, "[TCP Listener] Client %u tried to spoof userId %u\n",
                        client_id, input_msg.data.cancel.user_id);
                valid = false;
            }
            break;
    }
    
    if (valid) {
        // Create envelope and enqueue
    }
}
```

**This prevents:**
- Client A placing orders as Client B
- Client A cancelling Client B's orders
- Any form of client impersonation

### Auto-Cancel on Disconnect

When a TCP client disconnects, all their orders are automatically cancelled:

```c
// In main.c - graceful shutdown
uint32_t disconnected_clients[MAX_TCP_CLIENTS];
size_t num_disconnected = tcp_client_disconnect_all(client_registry,
                                                    disconnected_clients,
                                                    MAX_TCP_CLIENTS);

// Cancel all orders for each disconnected client
for (size_t i = 0; i < num_disconnected; i++) {
    processor_cancel_client_orders(&processor_ctx, disconnected_clients[i]);
}

// In order_book.c
size_t order_book_cancel_client_orders(order_book_t* book,
                                       uint32_t client_id,
                                       output_buffer_t* output) {
    size_t cancelled_count = 0;
    
    // Iterate all price levels and orders
    for (int i = 0; i < book->num_bid_levels; i++) {
        order_t* order = book->bids[i].orders_head;
        while (order != NULL) {
            order_t* next = order->next;
            if (order->client_id == client_id) {
                order_book_cancel_order(book, order->user_id, 
                                       order->user_order_id, output);
                cancelled_count++;
            }
            order = next;
        }
    }
    // Similar for asks...
    
    return cancelled_count;
}
```

---

## Message Framing (TCP)

### Wire Format

TCP streams don't have message boundaries, so we use length-prefixed framing:

```
┌────────────────────────────────────────────────────────────┐
│  4 bytes (big-endian)  │         N bytes                   │
│      Message Length    │       Message Payload             │
└────────────────────────────────────────────────────────────┘
```

**Example:**
```
CSV message: "N, 1, IBM, 100, 50, B, 1\n" (26 bytes)
Wire format: [0x00][0x00][0x00][0x1A]["N, 1, IBM, 100, 50, B, 1\n"]
```

### Framing Structures

```c
#define MAX_FRAMED_MESSAGE_SIZE 16384
#define FRAME_HEADER_SIZE 4

/* Read state - handles partial TCP reads */
typedef struct {
    char buffer[MAX_FRAMED_MESSAGE_SIZE];
    size_t buffer_pos;
    uint32_t expected_length;
    bool reading_header;    // true = reading length, false = reading body
} framing_read_state_t;

/* Write state - handles partial TCP writes */
typedef struct {
    char buffer[MAX_FRAMED_MESSAGE_SIZE + FRAME_HEADER_SIZE];
    size_t total_len;
    size_t written;
} framing_write_state_t;
```

### Framing API

```c
/* Initialize read state for new message */
void framing_read_state_init(framing_read_state_t* state);

/* Process incoming bytes, return true when complete message ready */
bool framing_read_process(framing_read_state_t* state,
                          const char* incoming_data,
                          size_t incoming_len,
                          const char** msg_data,
                          size_t* msg_len);

/* Initialize write state with framed message */
bool framing_write_state_init(framing_write_state_t* state,
                               const char* msg_data,
                               size_t msg_len);

/* Get remaining data to write */
void framing_write_get_remaining(framing_write_state_t* state,
                                 const char** data,
                                 size_t* len);

/* Mark bytes as written */
void framing_write_mark_written(framing_write_state_t* state,
                                size_t bytes_written);

/* Check if write complete */
bool framing_write_is_complete(framing_write_state_t* state);
```

### Usage Pattern

```c
// Reading
framing_read_state_t state;
framing_read_state_init(&state);

while (running) {
    ssize_t n = read(sock, buffer, sizeof(buffer));
    const char* msg;
    size_t msg_len;
    
    if (framing_read_process(&state, buffer, n, &msg, &msg_len)) {
        // Complete message ready - process it
        handle_message(msg, msg_len);
        framing_read_state_init(&state);  // Reset for next
    }
}

// Writing
framing_write_state_t state;
framing_write_state_init(&state, msg_data, msg_len);

while (!framing_write_is_complete(&state)) {
    const char* data;
    size_t len;
    framing_write_get_remaining(&state, &data, &len);
    
    ssize_t n = write(sock, data, len);
    if (n > 0) {
        framing_write_mark_written(&state, n);
    }
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

### 2. Envelope Pattern for Routing

**Decision:** Wrap messages with routing metadata

**Rationale:**
- **Separation of concerns** - Business logic doesn't know about TCP
- **Single queue scales** - O(1) dequeue regardless of client count
- **Flexible routing** - Trades go to multiple clients easily
- **Debugging** - Sequence numbers for tracing

**Trade-off:** Slightly larger message size (8 bytes overhead)

### 3. Two-Stage Queuing (TCP Mode)

**Decision:** Envelope queues → per-client queues

**Rationale:**
- **Fan-out** - One output can go to multiple clients
- **Backpressure** - Per-client queues prevent one slow client from blocking others
- **Isolation** - Client disconnection doesn't affect queue state

### 4. Anti-Spoofing Validation

**Decision:** Validate user_id == client_id

**Rationale:**
- **Security** - Clients can't impersonate others
- **Integrity** - Order ownership is guaranteed
- **Production-grade** - Essential for multi-tenant systems

### 5. Length-Prefixed Framing

**Decision:** 4-byte big-endian length prefix

**Rationale:**
- **Simple** - Easy to implement
- **Efficient** - Single length read
- **Compatible** - Works with both CSV and binary
- **Standard** - Common pattern in network protocols

### 6. epoll/kqueue Event Loop

**Decision:** Platform-specific event multiplexing

**Rationale:**
- **Scalable** - O(active fds), not O(total fds)
- **Non-blocking** - Single thread handles 100+ clients
- **Efficient** - No thread-per-client overhead

---

## Performance Characteristics

### Throughput

| Component | Throughput | Notes |
|-----------|-----------|-------|
| UDP Receiver | 1-10M pkts/sec | Network limited |
| TCP Receiver | 100K-1M msgs/sec | System call limited |
| Matching Engine | 1-5M orders/sec | CPU limited |
| Memory Pool Alloc | 100M ops/sec | Just index manipulation |
| Output Router | 1-5M msgs/sec | Lock-free queues |

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
| End-to-end (TCP) | 20-100μs | +framing overhead |

### Memory Usage

| Component | Memory | Notes |
|-----------|--------|-------|
| Order pool | ~1.5MB | 10K × 150 bytes |
| Hash entry pool | ~800KB | 10K × 80 bytes |
| Input envelope queue | ~3MB | 16K × 200 bytes |
| Output envelope queue | ~3MB | 16K × 200 bytes |
| Per-client queue | ~500KB | 4K × 128 bytes each |
| Order book (empty) | ~800KB | 2 × 10K × 40 bytes |
| Client registry | ~60MB | 100 × 600KB (incl queues) |
| **Total (typical)** | **70-100MB** | Predictable, pre-allocated |

---

## Project Structure

```
matching-engine-c/
├── include/                         # Header files
│   ├── core/
│   │   ├── order.h                  # Order structure
│   │   ├── order_book.h             # Order book + memory pools
│   │   └── matching_engine.h        # Multi-symbol orchestrator
│   ├── protocol/
│   │   ├── message_types.h          # Core message definitions
│   │   ├── message_types_extended.h # Envelope types
│   │   ├── csv/
│   │   │   ├── message_parser.h     # CSV input parser
│   │   │   └── message_formatter.h  # CSV output formatter
│   │   └── binary/
│   │       ├── binary_protocol.h    # Binary message specs
│   │       ├── binary_message_parser.h
│   │       └── binary_message_formatter.h
│   ├── network/
│   │   ├── tcp_listener.h           # TCP multi-client (epoll/kqueue)
│   │   ├── tcp_connection.h         # Per-client state + registry
│   │   ├── message_framing.h        # Length-prefix framing
│   │   └── udp_receiver.h           # UDP receiver
│   └── threading/
│       ├── lockfree_queue.h         # SPSC queue macros
│       ├── queues.h                 # Queue type declarations
│       ├── processor.h              # Processor thread
│       ├── output_router.h          # Output router (TCP)
│       └── output_publisher.h       # Output publisher (UDP)
│
├── src/                             # Implementation files
│   ├── main.c                       # Entry point (~400 lines)
│   ├── core/
│   │   ├── order_book.c             # Matching logic (~900 lines)
│   │   └── matching_engine.c        # Symbol routing (~200 lines)
│   ├── protocol/...
│   ├── network/
│   │   ├── tcp_listener.c           # Event loop (~600 lines)
│   │   ├── tcp_connection.c         # Client management
│   │   ├── message_framing.c        # Framing logic
│   │   └── udp_receiver.c
│   └── threading/
│       ├── processor.c              # Batch processing (~150 lines)
│       ├── output_router.c          # Per-client routing (~100 lines)
│       └── output_publisher.c
│
├── tools/                           # Test/debug tools
│   ├── binary_client.c
│   ├── tcp_client.c
│   └── binary_decoder.c
│
├── tests/                           # Unity test framework
│   ├── core/
│   ├── protocol/
│   └── scenarios/
│
├── documentation/                   # All documentation
│   ├── ARCHITECTURE.md              # This file
│   ├── BUILD.md
│   ├── PROTOCOLS.md
│   ├── QUICK_START.md
│   └── TESTING.md
│
├── build.sh                         # Build script
├── CMakeLists.txt                   # CMake configuration
└── README.md                        # Project overview
```

**Total:** ~6,000 lines of production code, ~2,000 lines of tests

---

## See Also

- [Quick Start Guide](QUICK_START.md) - Get up and running
- [Protocols](PROTOCOLS.md) - Message format specifications (CSV, Binary, TCP Framing)
- [Testing](TESTING.md) - Comprehensive testing guide
- [Build Instructions](BUILD.md) - Detailed build guide
