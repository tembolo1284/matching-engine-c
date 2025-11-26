# Testing Guide

Comprehensive testing guide for the Matching Engine covering unit tests, integration tests, multicast testing, and manual testing scenarios.

## Table of Contents
- [Overview](#overview)
- [Quick Test](#quick-test)
- [Unit Testing](#unit-testing)
  - [Unity Testing Framework](#unity-testing-framework)
  - [Test Components](#test-components)
  - [Memory Pool Tests](#memory-pool-tests)
  - [Lock-Free Queue Tests](#lock-free-queue-tests)
  - [Symbol Router Tests](#symbol-router-tests)
  - [Adding New Tests](#adding-new-tests)
- [Integration Testing](#integration-testing)
- [Dual-Processor Testing](#dual-processor-testing)
- [Multicast Testing](#multicast-testing)
- [Manual Testing Scenarios](#manual-testing-scenarios)
- [Protocol-Specific Testing](#protocol-specific-testing)
- [Multi-Client Testing](#multi-client-testing)
- [Performance Testing](#performance-testing)
- [Troubleshooting](#troubleshooting)

---

## Overview

The testing strategy includes:
- **Unit tests** - Unity framework for component testing (90 tests)
- **Integration tests** - End-to-end system tests
- **Manual tests** - Interactive testing via test clients
- **Protocol tests** - CSV and Binary protocol validation
- **Dual-processor tests** - Symbol partitioning verification
- **Multicast tests** - Market data broadcast verification
- **Performance tests** - Throughput and latency benchmarks

### Test Coverage

| Component | Unit Tests | Integration Tests |
|-----------|-----------|-------------------|
| Order Book | ✓ (11) | ✓ |
| Message Parser | ✓ (10) | ✓ |
| Message Formatter | ✓ (7) | ✓ |
| Matching Engine | ✓ (7) | ✓ |
| Binary Protocol | ✓ | ✓ |
| Memory Pools | ✓ (12) | ✓ |
| Lock-Free Queue | ✓ (18) | ✓ |
| Symbol Router | ✓ (19) | ✓ |
| Scenarios (1-16) | ✓ (6) | ✓ |
| TCP Multi-Client | ✗ | ✓ |
| UDP Mode | ✗ | ✓ |
| Dual-Processor | ✗ | ✓ |
| Multicast Publisher | ✗ | ✓ |
| Multicast Subscriber | ✗ | ✓ |

---

## Quick Test

### 30-Second Test
```bash
# Build
./build.sh build

# Run all unit tests
./build.sh test

# Expected output:
# ==========================================
# 90 Tests 0 Failures 0 Ignored
# OK
```

### 5-Minute Test
```bash
# Terminal 1: Start server (dual-processor is default)
./build/matching_engine --tcp

# Terminal 2: Run scenarios
./build/tcp_client localhost 1234 1
./build/tcp_client localhost 1234 2
./build/tcp_client localhost 1234 3
```

### Multicast Quick Test
```bash
# Terminal 1: Start server with multicast
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Terminal 2: Start subscriber
./build/multicast_subscriber 239.255.0.1 5000

# Terminal 3: Send orders
./build/tcp_client localhost 1234
> buy IBM 100 50 1
> sell IBM 100 30 2
```

**Result:** Subscriber receives all market data (acks, trades, TOB updates)!

---

## Unit Testing

### Unity Testing Framework

We use [Unity](https://github.com/ThrowTheSwitch/Unity) - a lightweight C testing framework designed for embedded systems.

**Features:**
- Simple assertions: `TEST_ASSERT_EQUAL(expected, actual)`
- Test isolation: Each test runs independently
- Detailed failure messages
- No external dependencies

### Running Unit Tests
```bash
# Build and run all unit tests
./build.sh test

# Run test executable directly
./build/matching_engine_tests

# Run with verbose output
./build/matching_engine_tests -v

# Run with valgrind (memory leak detection)
./build.sh valgrind
```

### Unit Test Output
```
==========================================
Running Unity Tests
==========================================

=== Message Formatter Tests ===
Running test_FormatAck... PASS
Running test_FormatCancelAck... PASS
...

=== Message Parser Tests ===
Running test_ParseNewOrderBuy... PASS
Running test_ParseNewOrderSell... PASS
...

=== Order Book Tests ===
Running test_AddSingleBuyOrder... PASS
Running test_MatchingBuyAndSell... PASS
...

=== Matching Engine Tests ===
Running test_ProcessSingleOrder... PASS
Running test_MultipleSymbols... PASS
...

=== Scenario Tests ===
Running test_Scenario1_BalancedBook... PASS
...

=== Memory Pool Tests ===
Running test_MemoryPools_InitializeCorrectly... PASS
Running test_MemoryPools_OrdersAllocateFromPool... PASS
...

=== Lock-Free Queue Tests ===
Running test_Queue_InitializesEmpty... PASS
Running test_Queue_FIFOOrder... PASS
...

=== Symbol Router Tests ===
Running test_SymbolRouter_AtoM_RoutesToProcessor0... PASS
Running test_SymbolRouter_NtoZ_RoutesToProcessor1... PASS
...

-----------------------
90 Tests 0 Failures 0 Ignored
OK
```

### Test Components

#### Order Book Tests (`tests/core/test_order_book.c`)
```c
// Test simple buy order
void test_AddSingleBuyOrder(void) {
    order_book_t* book = order_book_create("IBM");
    output_msg_t messages[10];
    int count = 0;
    
    order_book_add_order(book, 1, 100, 50, 'B', 1, &messages[0], &count);
    
    TEST_ASSERT_EQUAL(1, count);  // Should generate ack + TOB
    TEST_ASSERT_EQUAL(MSG_ACK, messages[0].type);
    
    order_book_destroy(book);
}

// Test matching orders
void test_MatchingBuyAndSell(void) {
    order_book_t* book = order_book_create("IBM");
    output_msg_t messages[10];
    int count = 0;
    
    // Add buy order
    order_book_add_order(book, 1, 100, 50, 'B', 1, messages, &count);
    
    // Add matching sell order
    count = 0;
    order_book_add_order(book, 2, 100, 50, 'S', 2, messages, &count);
    
    // Should generate: ack + trade + 2 TOB updates
    TEST_ASSERT_EQUAL(4, count);
    TEST_ASSERT_EQUAL(MSG_TRADE, messages[1].type);
    TEST_ASSERT_EQUAL(50, messages[1].data.trade.quantity);
    
    order_book_destroy(book);
}
```

#### Message Parser Tests (`tests/protocol/test_message_parser.c`)
```c
// Test CSV parsing
void test_ParseNewOrder(void) {
    input_msg_t msg;
    bool success = parse_message("N, 1, IBM, 100, 50, B, 1", &msg);
    
    TEST_ASSERT_TRUE(success);
    TEST_ASSERT_EQUAL(MSG_NEW_ORDER, msg.type);
    TEST_ASSERT_EQUAL(1, msg.data.new_order.user_id);
    TEST_ASSERT_EQUAL_STRING("IBM", msg.data.new_order.symbol);
    TEST_ASSERT_EQUAL(100, msg.data.new_order.price);
    TEST_ASSERT_EQUAL(50, msg.data.new_order.quantity);
    TEST_ASSERT_EQUAL('B', msg.data.new_order.side);
}

// Test malformed input
void test_ParseInvalidMessage(void) {
    input_msg_t msg;
    bool success = parse_message("N, ABC, IBM, 100, 50, B, 1", &msg);
    
    TEST_ASSERT_FALSE(success);  // Invalid user_id
}
```

#### Scenario Tests (`tests/scenarios/test_scenarios_odd.c`, `tests/scenarios/test_scenarios_even.c`)

Comprehensive end-to-end scenarios from the original specification:
```c
void test_Scenario1_BalancedBook(void) {
    // Place buy and sell orders at different prices
    // Verify no matching occurs
    // Test flush command
}

void test_Scenario2_MatchingOrders(void) {
    // Place buy order
    // Place matching sell order
    // Verify trade execution
    // Verify order removal from book
}

// ... Scenarios 3-16 ...
```

---

### Memory Pool Tests

Tests the **zero-allocation memory pool system** that provides O(1) alloc/free without malloc in the hot path.

**File:** `tests/core/test_memory_pools.c`

| Test | Purpose |
|------|---------|
| `test_MemoryPools_InitializeCorrectly` | Verifies pools start with zero allocations |
| `test_MemoryPools_TotalMemorySize` | Validates memory size calculation (>1MB) |
| `test_MemoryPools_OrdersAllocateFromPool` | Tests allocation tracking via order book |
| `test_MemoryPools_CancelReturnsToPool` | Verifies cancellation returns slots to pool |
| `test_MemoryPools_FlushReturnsAllToPool` | Tests bulk deallocation on flush |
| `test_MemoryPools_TradeRemovesOrders` | Verifies trade clears matched orders |
| `test_MemoryPools_PeakUsageTracking` | Tests high-water mark tracking |
| `test_MemoryPools_SharedByMultipleBooks` | Tests multiple order books sharing pools |
| `test_MemoryPools_HighVolumeOperations` | Stress test: 1000 alloc/free cycles |
| `test_MemoryPools_DataIntegrityUnderReuse` | Validates no corruption on slot reuse |
| `test_MemoryPools_EmptyPoolsZeroStats` | Edge case: fresh pool statistics |
| `test_MemoryPools_ReInitResets` | Tests pool re-initialization |

**Example Test:**
```c
void test_MemoryPools_CancelReturnsToPool(void) {
    setUp();
    
    order_book_init(&book, "TEST", &pools);
    output_buffer_t output;
    
    /* Add order */
    new_order_msg_t order = {1, "TEST", 100, 50, SIDE_BUY, 1};
    output_buffer_init(&output);
    order_book_add_order(&book, &order, 1, &output);
    
    memory_pool_stats_t stats_before;
    memory_pools_get_stats(&pools, &stats_before);
    uint32_t peak_before = stats_before.order_peak_usage;
    
    /* Cancel order */
    output_buffer_init(&output);
    order_book_cancel_order(&book, 1, 1, &output);
    
    /* Add another order - should reuse freed slot */
    new_order_msg_t order2 = {1, "TEST", 101, 50, SIDE_BUY, 2};
    output_buffer_init(&output);
    order_book_add_order(&book, &order2, 1, &output);
    
    memory_pool_stats_t stats_after;
    memory_pools_get_stats(&pools, &stats_after);
    
    /* Peak should not have increased (slot was reused) */
    TEST_ASSERT_EQUAL(peak_before, stats_after.order_peak_usage);
    
    order_book_destroy(&book);
    tearDown();
}
```

**Key Verification Points:**
- Pool statistics track allocations, peak usage, and failures
- Cancelled orders return slots to the free list
- Slots are reused without increasing peak usage
- No allocation failures under normal load

---

### Lock-Free Queue Tests

Tests the **SPSC (Single-Producer Single-Consumer) lock-free queue** used for inter-thread communication.

**File:** `tests/threading/test_lockfree_queue.c`

| Test | Purpose |
|------|---------|
| `test_Queue_InitializesEmpty` | Verifies empty state after init |
| `test_Queue_CorrectCapacity` | Validates capacity = LOCKFREE_QUEUE_SIZE |
| `test_Queue_StatsZeroedOnInit` | Checks all stats start at zero |
| `test_Queue_SingleEnqueueDequeue` | Basic single-item operation |
| `test_Queue_MultipleEnqueueDequeue` | Batch enqueue then dequeue (100 items) |
| `test_Queue_InterleavedOperations` | Mixed enqueue/dequeue sequence |
| `test_Queue_FIFOOrder` | Verifies strict FIFO ordering (1000 items) |
| `test_Queue_DequeueFromEmptyFails` | Empty queue returns false, tracks failure |
| `test_Queue_EnqueueToFullFails` | Full queue returns false, tracks failure |
| `test_Queue_WrapAround` | Tests ring buffer wrap-around (5 cycles) |
| `test_Queue_EnqueueStatsTracked` | Validates total_enqueues counter |
| `test_Queue_DequeueStatsTracked` | Validates total_dequeues counter |
| `test_Queue_PeakSizeTracked` | Tests peak size high-water mark |
| `test_Queue_InvariantsHold` | Validates internal consistency |
| `test_Queue_NullQueueHandling` | NULL queue pointer safety |
| `test_Queue_NullItemHandling` | NULL item pointer safety |
| `test_Queue_DataIntegrity` | All struct fields preserved through queue |
| `test_Queue_SizeConsistency` | Size matches enqueue/dequeue operations |

**Example Test:**
```c
void test_Queue_FIFOOrder(void) {
    setUp();
    
    test_item_t item;
    const int count = 1000;
    
    /* Enqueue items with sequential IDs */
    for (int i = 0; i < count; i++) {
        item.id = (uint32_t)i;
        item.value = (uint32_t)(i * 7);
        TEST_ASSERT_TRUE(test_queue_enqueue(&queue, &item));
    }
    
    /* Dequeue and verify strict FIFO order */
    for (int i = 0; i < count; i++) {
        TEST_ASSERT_TRUE(test_queue_dequeue(&queue, &item));
        TEST_ASSERT_EQUAL_MESSAGE((uint32_t)i, item.id, "FIFO order violated");
        TEST_ASSERT_EQUAL((uint32_t)(i * 7), item.value);
    }
    
    tearDown();
}
```

**Key Verification Points:**
- FIFO ordering is strictly preserved
- Ring buffer handles wrap-around correctly
- Boundary conditions (empty/full) handled gracefully
- Statistics accurately track operations
- Data integrity maintained through enqueue/dequeue cycle

**Note:** These are single-threaded correctness tests. Multi-threaded stress testing requires a separate harness with proper synchronization.

---

### Symbol Router Tests

Tests the **symbol-based processor routing** that partitions orders between dual processors.

**File:** `tests/protocol/test_symbol_router.c`

**Routing Rules:**
- Symbols starting with **A-M** → Processor 0
- Symbols starting with **N-Z** → Processor 1
- Empty/invalid/numeric → Processor 0 (default)

| Test | Purpose |
|------|---------|
| `test_SymbolRouter_AtoM_RoutesToProcessor0` | AAPL, IBM, META → P0 |
| `test_SymbolRouter_NtoZ_RoutesToProcessor1` | NVDA, TSLA, ZM → P1 |
| `test_SymbolRouter_M_BoundaryProcessor0` | M is last letter → P0 |
| `test_SymbolRouter_N_BoundaryProcessor1` | N is first letter → P1 |
| `test_SymbolRouter_A_StartBoundary` | A boundary (start of range) |
| `test_SymbolRouter_Z_EndBoundary` | Z boundary (end of range) |
| `test_SymbolRouter_Lowercase_AtoM` | aapl, ibm → P0 |
| `test_SymbolRouter_Lowercase_NtoZ` | nvda, tsla → P1 |
| `test_SymbolRouter_MixedCase` | iBm → P0, tSlA → P1 |
| `test_SymbolRouter_NullSymbol` | NULL → P0 (safe default) |
| `test_SymbolRouter_EmptySymbol` | "" → P0 (safe default) |
| `test_SymbolRouter_NumericSymbol` | "1234" → P0 (default) |
| `test_SymbolRouter_SpecialCharSymbol` | "$SPX", ".DJI" → P0 |
| `test_SymbolRouter_SingleCharSymbols` | "A", "M", "N", "Z" |
| `test_SymbolIsValid_ValidSymbols` | Validation function (valid) |
| `test_SymbolIsValid_InvalidSymbols` | Validation function (invalid) |
| `test_GetProcessorName` | "A-M", "N-Z", "Unknown" |
| `test_SymbolRouter_Consistency` | Same symbol → same processor (100x) |
| `test_SymbolRouter_ValidProcessorIds` | IDs in range [0, NUM_PROCESSORS) |

**Example Test:**
```c
void test_SymbolRouter_M_BoundaryProcessor0(void) {
    /* M is the LAST letter that routes to Processor 0 */
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("M"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("MSFT"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_A_TO_M, get_processor_id_for_symbol("MCD"));
}

void test_SymbolRouter_N_BoundaryProcessor1(void) {
    /* N is the FIRST letter that routes to Processor 1 */
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("N"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("NFLX"));
    TEST_ASSERT_EQUAL(PROCESSOR_ID_N_TO_Z, get_processor_id_for_symbol("NKE"));
}
```

**Key Verification Points:**
- M/N boundary is the critical partition point
- Case normalization works correctly
- Edge cases (NULL, empty, numeric) have safe defaults
- Routing is deterministic and consistent

---

### Adding New Tests

1. Create test file in appropriate `tests/` subdirectory:
```c
#include "unity.h"
#include "your_header.h"

void setUp(void) {
    // Run before each test
}

void tearDown(void) {
    // Run after each test
}

void test_YourTestName(void) {
    // Test code
    TEST_ASSERT_EQUAL(expected, actual);
}
```

2. Add external declaration to `tests/test_runner.c`:
```c
extern void test_YourTestName(void);
```

3. Add RUN_TEST call in main():
```c
printf("\n=== Your Test Category ===\n");
RUN_TEST(test_YourTestName);
```

4. Add source file to `CMakeLists.txt`:
```cmake
set(TEST_SOURCES
    ...
    tests/your_category/test_your_component.c
    ...
)
```

5. Rebuild and run:
```bash
./build.sh build
./build.sh test
```

---

## Integration Testing

### TCP Integration Test

Automated TCP server/client testing:
```bash
./build.sh test-tcp
```

**What it tests:**
- TCP connection establishment
- Message framing (length-prefix)
- Order placement and matching
- Multi-client isolation
- Graceful shutdown

**Output:**
```
==========================================
TCP Integration Test
==========================================
Starting TCP server on port 1234...
Server PID: 12345
Waiting for server to start...

Running test scenario...
Connecting to localhost:1234
Sent: N, 1, IBM, 100, 50, B, 1
Received: A, IBM, 1, 1
Sent: N, 1, IBM, 100, 50, S, 2
Received: T, IBM, 1, 1, 1, 2, 100, 50

✓ TCP integration test passed
Stopping server...
```

### Binary Protocol Test

Automated binary protocol validation:
```bash
./build.sh test-binary        # UDP mode, CSV output
./build.sh test-binary-full   # UDP mode, binary output with decoder
```

**What it tests:**
- Binary message encoding
- Protocol auto-detection
- Binary parsing
- Output formatting (CSV or binary)

**Output:**
```
==========================================
Binary Protocol Test - CSV Output
==========================================
Starting server (CSV output mode)...
Sending binary test messages...

Sent: NEW IBM B 50 @ 100 (order 1)
Sent: NEW IBM S 50 @ 105 (order 2)
Sent: FLUSH

Server output:
A, IBM, 1, 1
B, IBM, B, 100, 50
A, IBM, 2, 2
B, IBM, S, 105, 50
C, IBM, 1, 1
C, IBM, 2, 2

✓ Binary protocol test complete
```

### Run All Tests
```bash
./build.sh test       # Unit tests
./build.sh test-all   # All automated tests (prints instructions for manual tests)
```

---

## Dual-Processor Testing

The matching engine supports dual-processor mode for horizontal scaling. Orders are partitioned by symbol:
- **Processor 0**: Symbols A-M (AAPL, IBM, GOOGL, META, etc.)
- **Processor 1**: Symbols N-Z (NVDA, TSLA, UBER, ZM, etc.)

### Quick Dual-Processor Test
```bash
# Terminal 1: Start server in dual-processor mode (default)
./build/matching_engine --tcp

# Terminal 2: Send orders to both processors
./build/tcp_client localhost 1234
> buy IBM 100 50 1      # → Processor 0 (I is A-M)
> buy NVDA 200 25 2     # → Processor 1 (N is N-Z)
> buy AAPL 150 30 3     # → Processor 0 (A is A-M)
> buy TSLA 180 40 4     # → Processor 1 (T is N-Z)
> flush
> quit
```

### Verifying Symbol Routing

When the server shuts down (Ctrl+C), it prints per-processor statistics:
```
=== Processor 0 (A-M) Statistics ===
  Messages processed: 2
  Orders matched: 0
  
=== Processor 1 (N-Z) Statistics ===
  Messages processed: 2
  Orders matched: 0

=== TCP Listener Statistics ===
  Messages to Processor 0: 2
  Messages to Processor 1: 2
```

### Symbol Routing Reference

| Symbol | First Letter | Processor |
|--------|--------------|-----------|
| AAPL   | A            | 0 (A-M)   |
| IBM    | I            | 0 (A-M)   |
| GOOGL  | G            | 0 (A-M)   |
| META   | M            | 0 (A-M)   |
| NVDA   | N            | 1 (N-Z)   |
| TSLA   | T            | 1 (N-Z)   |
| UBER   | U            | 1 (N-Z)   |
| ZM     | Z            | 1 (N-Z)   |

### Single-Processor Mode

For comparison or debugging, run in single-processor mode:
```bash
# Single processor (all symbols to one processor)
./build/matching_engine --tcp --single-processor

# Dual processor (default)
./build/matching_engine --tcp --dual-processor
./build/matching_engine --tcp  # Same as above
```

### Dual-Processor Test Scenarios

#### Scenario 1: Cross-Processor Independence

Verify that orders on different symbols don't interact:
```bash
# Terminal 2:
> buy IBM 100 50 1      # Processor 0
> sell NVDA 100 50 2    # Processor 1 - should NOT match IBM order
> flush
```

**Expected:** No trades (different symbols, different processors)

#### Scenario 2: Same-Processor Matching

Verify matching works within a processor:
```bash
# Terminal 2:
> buy IBM 100 50 1      # Processor 0
> sell IBM 100 50 2     # Processor 0 - should match
> flush
```

**Expected:** Trade generated (same symbol, same processor)

#### Scenario 3: Flush Affects Both Processors
```bash
# Terminal 2:
> buy IBM 100 50 1      # Processor 0
> buy NVDA 200 25 2     # Processor 1
> flush                 # Should cancel orders in BOTH processors
```

**Expected:** Both orders cancelled, both processors show flush statistics

#### Scenario 4: High-Volume Routing
```bash
# Send many orders to verify routing under load
for i in {1..100}; do
    echo "N, 1, IBM, 100, 50, B, $i"    # All to Processor 0
done | nc localhost 1234

for i in {101..200}; do
    echo "N, 1, TSLA, 100, 50, B, $i"   # All to Processor 1
done | nc localhost 1234
```

**Expected:** ~100 messages to each processor (check shutdown statistics)

### Automated Dual-Processor Test
```bash
# Run automated dual-processor test
./build.sh test-dual-processor
```

This test:
1. Starts the server in dual-processor mode
2. Sends orders to A-M symbols (Processor 0)
3. Sends orders to N-Z symbols (Processor 1)
4. Verifies per-processor statistics
5. Confirms no cross-processor interference

---

## Multicast Testing

The matching engine supports UDP multicast for market data distribution. This is the **industry-standard pattern** used by real exchanges (CME, NASDAQ, ICE).

### Why Test Multicast?

Multicast enables:
- **Zero per-subscriber overhead** - One send reaches unlimited subscribers
- **Real-time market data** - All subscribers receive data simultaneously
- **Scalability** - 1 subscriber or 1000 subscribers = same server cost

### Quick Multicast Test
```bash
# Interactive test with instructions
./build.sh test-multicast
```

This provides step-by-step instructions for a 3-terminal test.

### Manual Multicast Test

#### Basic Test (3 Terminals)
```bash
# Terminal 1: Start server with multicast
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Terminal 2: Start subscriber
./build/multicast_subscriber 239.255.0.1 5000

# Terminal 3: Send orders via TCP
./build/tcp_client localhost 1234
> buy IBM 100 50 1
> sell IBM 100 30 2
> flush
> quit
```

**Expected Subscriber Output:**
```
[Multicast Subscriber] Joined group 239.255.0.1:5000
[Multicast Subscriber] Waiting for market data...

[CSV] A, IBM, 1, 1
[CSV] B, IBM, B, 100, 50
[CSV] A, IBM, 1, 2
[CSV] T, IBM, 1, 1, 1, 2, 100, 30
[CSV] B, IBM, B, 100, 20
[CSV] B, IBM, S, -, -
[CSV] C, IBM, 1, 1
[CSV] B, IBM, B, -, -

^C
--- Statistics ---
Packets received: 8
Messages: 8
Elapsed: 12.345s
Throughput: 0.65 msg/sec
```

#### Multiple Subscribers Test

**Purpose:** Verify all subscribers receive the same data simultaneously.
```bash
# Terminal 1: Server with multicast
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Terminal 2: Subscriber 1
./build/multicast_subscriber 239.255.0.1 5000

# Terminal 3: Subscriber 2
./build/multicast_subscriber 239.255.0.1 5000

# Terminal 4: Subscriber 3
./build/multicast_subscriber 239.255.0.1 5000

# Terminal 5: Send orders
./build/tcp_client localhost 1234
> buy IBM 100 50 1
> sell IBM 100 50 2
```

**Expected:** ALL three subscribers receive identical market data simultaneously!

#### Binary Multicast Test
```bash
# Terminal 1: Server with binary multicast
./build/matching_engine --tcp --binary --multicast 239.255.0.1:5000

# Terminal 2: Subscriber (auto-detects binary)
./build/multicast_subscriber 239.255.0.1 5000

# Terminal 3: Send orders
./build/tcp_client localhost 1234
> buy IBM 100 50 1
```

**Expected Subscriber Output:**
```
[Multicast Subscriber] Joined group 239.255.0.1:5000
[Multicast Subscriber] Waiting for market data...

[BINARY] Ack: symbol=IBM, userId=1, orderId=1
[BINARY] TOB: symbol=IBM, side=B, price=100, qty=50
```

### Multicast Test Scenarios

#### Scenario 1: Subscriber Joins Late

**Purpose:** Verify late-joining subscribers receive subsequent messages.
```bash
# Terminal 1: Start server
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Terminal 2: Send some orders FIRST
./build/tcp_client localhost 1234
> buy IBM 100 50 1

# Terminal 3: Start subscriber AFTER orders sent
./build/multicast_subscriber 239.255.0.1 5000

# Terminal 2: Send more orders
> buy AAPL 150 30 2
```

**Expected:** Subscriber receives AAPL order messages (not IBM - those were sent before joining).

#### Scenario 2: Subscriber Disconnect/Reconnect

**Purpose:** Verify subscriber can rejoin multicast group.
```bash
# Terminal 2: Start subscriber
./build/multicast_subscriber 239.255.0.1 5000

# Press Ctrl+C to disconnect
# Restart subscriber
./build/multicast_subscriber 239.255.0.1 5000
```

**Expected:** Subscriber successfully rejoins and receives new messages.

#### Scenario 3: High-Volume Multicast

**Purpose:** Test multicast under load.
```bash
# Terminal 1: Server with multicast
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Terminal 2: Subscriber
./build/multicast_subscriber 239.255.0.1 5000

# Terminal 3: Generate high volume
for i in {1..1000}; do
    echo "N, 1, IBM, 100, 50, B, $i"
done | nc localhost 1234

echo "F" | nc localhost 1234
```

**Expected:** Subscriber receives all 1000+ messages (acks + TOB updates + cancel acks).

#### Scenario 4: Dual-Processor + Multicast

**Purpose:** Verify multicast receives from both processors.
```bash
# Terminal 1: Server (dual-processor + multicast)
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Terminal 2: Subscriber
./build/multicast_subscriber 239.255.0.1 5000

# Terminal 3: Send to both processors
./build/tcp_client localhost 1234
> buy IBM 100 50 1      # Processor 0
> buy NVDA 200 25 2     # Processor 1
> flush
```

**Expected:** Subscriber receives messages from BOTH processors (IBM from P0, NVDA from P1).

### Multicast Statistics

When the server shuts down (Ctrl+C), it prints multicast statistics:
```
=== Multicast Publisher Statistics ===
  Packets sent: 1234
  Messages broadcast: 1234
  Messages from Processor 0: 617
  Messages from Processor 1: 617
  Send errors: 0
```

When subscriber exits (Ctrl+C):
```
--- Statistics ---
Packets received: 1234
Messages: 1234
Binary messages: 0
CSV messages: 1234
Parse errors: 0
Elapsed: 60.123s
Throughput: 20.52 msg/sec
```

### Multicast Network Requirements

| Requirement | Description |
|-------------|-------------|
| **Multicast-enabled network** | Most modern LANs support multicast |
| **Same subnet** | Default TTL=1 (local subnet only) |
| **No firewall blocking** | UDP port 5000 must be open |
| **IGMP snooping** | Switch should support IGMP for efficiency |

### Troubleshooting Multicast

#### Subscriber not receiving data
```bash
# Check if multicast is working on loopback
./build/matching_engine --tcp --multicast 239.255.0.1:5000
./build/multicast_subscriber 239.255.0.1 5000
# If this works, network may be blocking multicast

# Check multicast routes (Linux)
ip maddr show

# Add multicast route if missing (Linux)
sudo ip route add 239.0.0.0/8 dev eth0
```

#### "Cannot assign requested address"
```bash
# Network interface doesn't support multicast
# Try loopback interface or check network config
```

#### Packet loss under high load
```bash
# Increase receive buffer (in subscriber code)
int buffer_size = 10 * 1024 * 1024;  // 10MB
setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
```

---

## Manual Testing Scenarios

### Scenario 1: Simple Orders (No Match)

**Purpose:** Test order book building without trades.
```bash
# Terminal 1: Server
./build/matching_engine --tcp

# Terminal 2: Client
./build/tcp_client localhost 1234

# Commands:
> buy IBM 100 50 1
> sell IBM 105 50 2
> flush
> quit
```

**Expected output:**
```
A, IBM, 1, 1
B, IBM, B, 100, 50
A, IBM, 2, 2
B, IBM, S, 105, 50
C, IBM, 1, 1
C, IBM, 2, 2
B, IBM, B, -, -
B, IBM, S, -, -
```

### Scenario 2: Trade Execution

**Purpose:** Test matching and trade generation.
```bash
# Terminal 1: Server
./build/matching_engine --tcp

# Terminal 2: Client
./build/tcp_client localhost 1234

# Commands:
> buy IBM 100 50 1
> sell IBM 100 50 2
> flush
> quit
```

**Expected output:**
```
A, IBM, 1, 1
B, IBM, B, 100, 50
A, IBM, 2, 2
T, IBM, 1, 1, 2, 2, 100, 50
B, IBM, B, -, -
B, IBM, S, -, -
```

### Scenario 3: Order Cancellation

**Purpose:** Test cancel functionality.
```bash
# Terminal 1: Server
./build/matching_engine --tcp

# Terminal 2: Client
./build/tcp_client localhost 1234

# Commands:
> buy IBM 100 50 1
> cancel 1
> flush
> quit
```

**Expected output:**
```
A, IBM, 1, 1
B, IBM, B, 100, 50
C, IBM, 1, 1
B, IBM, B, -, -
```

### Scenario 4: Partial Fills

**Purpose:** Test partial order matching.
```bash
# Commands:
> buy IBM 100 100 1
> sell IBM 100 50 2
> sell IBM 100 50 3
> flush
```

**Expected:**
- First sell fills 50 shares, leaving 50 on buy order
- Second sell fills remaining 50 shares
- Two trade messages generated

### Scenario 5: Price-Time Priority

**Purpose:** Verify FIFO at same price level.
```bash
# Commands:
> buy IBM 100 50 1
> buy IBM 100 50 2
> sell IBM 100 75 3
> flush
```

**Expected:**
- Order 1 gets fully filled (50 shares)
- Order 2 gets partially filled (25 shares)
- Order with earlier timestamp (order 1) trades first

---

## Protocol-Specific Testing

### Testing CSV Protocol
```bash
# Terminal 1: Server with CSV output
./build/matching_engine --tcp 1234

# Terminal 2: Client sends CSV
./build/tcp_client localhost 1234
> N, 1, IBM, 100, 50, B, 1
> N, 1, IBM, 100, 50, S, 2
> F
> quit
```

### Testing Binary Protocol
```bash
# Terminal 1: Server with binary output + decoder
./build/matching_engine --tcp --binary 2>/dev/null | ./build/binary_decoder

# Terminal 2: Client sends binary
./build/binary_client 1234 2 --tcp
```

### Testing Mixed Protocols
```bash
# Terminal 1: Server accepts both
./build/matching_engine --tcp 1234

# Terminal 2: CSV client
./build/tcp_client localhost 1234
> N, 1, IBM, 100, 50, B, 1

# Terminal 3: Binary client
./build/binary_client 1234 --tcp
# (Binary commands)
```

Server auto-detects and handles both!

### Testing Multicast with Binary
```bash
# Terminal 1: Server with binary multicast
./build/matching_engine --tcp --binary --multicast 239.255.0.1:5000

# Terminal 2: Subscriber (auto-detects binary)
./build/multicast_subscriber 239.255.0.1 5000

# Terminal 3: Send orders (CSV or binary - server converts to binary output)
./build/tcp_client localhost 1234
> buy IBM 100 50 1
```

---

## Multi-Client Testing

### Two-Client Trade

**Purpose:** Verify trade notifications reach both clients.
```bash
# Terminal 1: Server
./build/matching_engine --tcp

# Terminal 2: Client 1 (will be assigned client_id=1)
./build/tcp_client localhost 1234
> N, 1, IBM, 100, 50, B, 1

# Terminal 3: Client 2 (will be assigned client_id=2)
./build/tcp_client localhost 1234
> N, 2, IBM, 100, 50, S, 1
```

**Expected:**
- Client 1 receives: Trade message
- Client 2 receives: Ack + Trade message
- Both see same trade details

### Three-Client Scenario
```bash
# Terminal 1: Server
./build/matching_engine --tcp

# Terminal 2: Client 1 (buyer)
./build/tcp_client localhost 1234
> N, 1, IBM, 100, 50, B, 1

# Terminal 3: Client 2 (seller)
./build/tcp_client localhost 1234
> N, 2, IBM, 100, 25, S, 1

# Terminal 4: Client 3 (seller)
./build/tcp_client localhost 1234
> N, 3, IBM, 100, 25, S, 1
```

**Expected:**
- Client 1 receives: 2 trade messages (partial fills)
- Client 2 receives: Ack + Trade message
- Client 3 receives: Ack + Trade message

### Client Isolation Test

**Purpose:** Verify clients cannot spoof other clients' user_ids.
```bash
# Terminal 2: Client 1 (assigned client_id=1)
./build/tcp_client localhost 1234
> N, 2, IBM, 100, 50, B, 1  # Try to use userId=2
```

**Expected:**
Server rejects with error: "User ID mismatch"

### Multi-Client + Multicast

**Purpose:** Verify TCP clients AND multicast subscribers all receive data.
```bash
# Terminal 1: Server with multicast
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Terminal 2: TCP Client 1
./build/tcp_client localhost 1234
> buy IBM 100 50 1

# Terminal 3: TCP Client 2
./build/tcp_client localhost 1234
> sell IBM 100 50 1

# Terminal 4: Multicast subscriber
./build/multicast_subscriber 239.255.0.1 5000
```

**Expected:**
- TCP Client 1 receives: Ack + Trade (via TCP)
- TCP Client 2 receives: Ack + Trade (via TCP)
- Multicast subscriber receives: ALL messages (Acks + Trade + TOB updates)

---

## Performance Testing

### Throughput Test
```bash
# Generate high message volume
for i in {1..1000}; do
    echo "N, 1, IBM, 100, 50, B, $i"
done | nc localhost 1234
```

### Dual-Processor Throughput Test
```bash
# Test parallel throughput across both processors
# Terminal 1: Server
./build/matching_engine --tcp

# Terminal 2: Processor 0 load (A-M symbols)
for i in {1..1000}; do echo "N, 1, IBM, 100, 50, B, $i"; done | nc localhost 1234 &

# Terminal 3: Processor 1 load (N-Z symbols)  
for i in {1001..2000}; do echo "N, 1, TSLA, 100, 50, B, $i"; done | nc localhost 1234 &

wait
```

**Expected:** Near-linear scaling (2x throughput vs single-processor)

### Multicast Throughput Test
```bash
# Terminal 1: Server with multicast
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Terminal 2: Subscriber (watch throughput)
./build/multicast_subscriber 239.255.0.1 5000

# Terminal 3: Generate load
for i in {1..10000}; do
    echo "N, 1, IBM, 100, 50, B, $i"
done | nc localhost 1234

echo "F" | nc localhost 1234
```

**Expected subscriber output:**
```
--- Statistics ---
Packets received: 20001
Messages: 20001
Elapsed: 5.234s
Throughput: 3822.15 msg/sec
```

### Latency Test

Use the built-in statistics:
```bash
./build/matching_engine --tcp
# Run tests
# Ctrl+C to see statistics

=== Final Statistics ===
Processor 0 (A-M):
  Messages processed: 10000
  Batches processed: 313
  Average batch size: 31.95

Processor 1 (N-Z):
  Messages processed: 10000
  Batches processed: 315
  Average batch size: 31.75
```

### Memory Test
```bash
# Run with valgrind
./build.sh valgrind

# Or manually
valgrind --leak-check=full ./build/matching_engine --tcp
```

---

## Troubleshooting

### Test Failures

#### "Address already in use"
```bash
# Kill existing server
pkill matching_engine

# Or use different port
./build/matching_engine --tcp 5000
```

#### "Connection refused"
```bash
# Ensure server is running
ps aux | grep matching_engine

# Check server logs
./build/matching_engine --tcp 2>&1 | tee server.log
```

#### "Test timeout"
```bash
# Increase timeout in test script
# Or check server responsiveness
telnet localhost 1234
```

### Dual-Processor Issues

#### Orders not routing correctly
```bash
# Verify symbol routing logic
# First character determines processor:
# A-M (ASCII 65-77) → Processor 0
# N-Z (ASCII 78-90) → Processor 1

# Check shutdown statistics for message counts per processor
```

#### Flush not cancelling all orders
```bash
# Flush should be sent to BOTH processors
# Check that both processors show flush in statistics
```

### Multicast Issues

#### Subscriber not receiving data
```bash
# Check if server is sending to multicast
# Server should show: [Multicast Publisher] Starting...

# Check multicast group membership
netstat -gn  # Linux
netstat -g   # macOS

# Try on loopback first
./build/matching_engine --tcp --multicast 239.255.0.1:5000
./build/multicast_subscriber 239.255.0.1 5000
```

#### "Cannot assign requested address"
```bash
# Network doesn't support multicast
# Check interface supports multicast:
ip link show eth0 | grep MULTICAST
```

#### Packet loss
```bash
# Subscriber shows fewer messages than server sent
# Increase buffer size or reduce send rate
```

### Memory Leaks
```bash
# Run valgrind on tests
./build.sh valgrind

# Run valgrind on server
valgrind --leak-check=full --show-leak-kinds=all \
    ./build/matching_engine --tcp
```

### Binary Protocol Issues

#### Wrong endianness
```bash
# Verify network byte order
./build/binary_decoder < test_message.bin

# Should show correct values
```

#### Truncated messages
```bash
# Check message sizes
hexdump -C test_message.bin | head
# Verify matches protocol spec
```

---

## Continuous Testing

### Pre-commit Hook

Create `.git/hooks/pre-commit`:
```bash
#!/bin/bash
./build.sh build
./build.sh test

if [ $? -ne 0 ]; then
    echo "Tests failed. Commit aborted."
    exit 1
fi
```

### CI/CD Integration

Example GitHub Actions workflow:
```yaml
name: Test
on: [push, pull_request]
jobs:
  test:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      - name: Install dependencies
        run: sudo apt-get install build-essential cmake ninja-build valgrind
      - name: Build
        run: ./build.sh build
      - name: Test
        run: ./build.sh test
      - name: Test dual-processor
        run: ./build.sh test-dual-processor
      - name: Valgrind
        run: ./build.sh valgrind
```

---

## Test Coverage Summary

| Category | Unit Tests | Integration | Notes |
|----------|------------|-------------|-------|
| Message Formatter | 7 | ✓ | CSV output formatting |
| Message Parser | 10 | ✓ | CSV input parsing |
| Order Book | 11 | ✓ | Core matching logic |
| Matching Engine | 7 | ✓ | Multi-symbol coordination |
| Scenarios | 6 | ✓ | End-to-end scenarios |
| Memory Pools | 12 | ✓ | Zero-allocation system |
| Lock-Free Queue | 18 | ✓ | SPSC queue correctness |
| Symbol Router | 19 | ✓ | Dual-processor routing |
| TCP Multi-Client | - | ✓ | Connection handling |
| Binary Protocol | - | ✓ | Binary encoding/decoding |
| Dual-Processor | - | ✓ | Symbol partitioning |
| Multicast | - | ✓ | Market data broadcast |
| **Total** | **90** | ✓ | |

---

## Quick Reference

### Test Commands
```bash
./build.sh test                 # Unit tests (90 tests)
./build.sh test-tcp             # TCP integration
./build.sh test-binary          # Binary protocol test
./build.sh test-binary-full     # Binary with decoder
./build.sh test-dual-processor  # Dual-processor routing test
./build.sh test-single-processor # Single-processor mode test
./build.sh test-multicast       # Multicast market data feed
./build.sh test-all             # Print all test instructions
./build.sh valgrind             # Memory leak detection
```

### Manual Test Scenarios
```bash
./build/tcp_client localhost 1234 1  # Simple orders
./build/tcp_client localhost 1234 2  # Trade execution
./build/tcp_client localhost 1234 3  # Cancellation
```

### Protocol Testing
```bash
# CSV
./build/matching_engine --tcp
./build/tcp_client localhost 1234

# Binary
./build/matching_engine --tcp --binary | ./build/binary_decoder
./build/binary_client 1234 --tcp
```

### Processor Mode Testing
```bash
# Dual-processor (default)
./build/matching_engine --tcp

# Single-processor
./build/matching_engine --tcp --single-processor
```

### Multicast Testing
```bash
# Server with multicast
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Subscriber (can run multiple)
./build/multicast_subscriber 239.255.0.1 5000

# Binary multicast
./build/matching_engine --tcp --binary --multicast 239.255.0.1:5000
```

---

## See Also

- [Quick Start Guide](QUICK_START.md) - Basic usage
- [Architecture](ARCHITECTURE.md) - System design including multicast
- [Protocols](PROTOCOLS.md) - Message formats and multicast protocol
- [Build Instructions](BUILD.md) - CMake build setup
