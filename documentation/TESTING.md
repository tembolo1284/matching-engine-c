# Testing Guide

Comprehensive testing guide for the Matching Engine covering unit tests, integration tests, and manual testing scenarios.

## Table of Contents
- [Overview](#overview)
- [Quick Test](#quick-test)
- [Unit Testing](#unit-testing)
- [Integration Testing](#integration-testing)
- [Manual Testing Scenarios](#manual-testing-scenarios)
- [Protocol-Specific Testing](#protocol-specific-testing)
- [Multi-Client Testing](#multi-client-testing)
- [Performance Testing](#performance-testing)
- [Troubleshooting](#troubleshooting)

---

## Overview

The testing strategy includes:
- **Unit tests** - Unity framework for component testing
- **Integration tests** - End-to-end system tests
- **Manual tests** - Interactive testing via test clients
- **Protocol tests** - CSV and Binary protocol validation
- **Performance tests** - Throughput and latency benchmarks

### Test Coverage

| Component | Unit Tests | Integration Tests |
|-----------|-----------|-------------------|
| Order Book | ✓ | ✓ |
| Message Parser | ✓ | ✓ |
| Message Formatter | ✓ | ✓ |
| Matching Engine | ✓ | ✓ |
| Binary Protocol | ✓ | ✓ |
| TCP Multi-Client | ✗ | ✓ |
| UDP Mode | ✗ | ✓ |

---

## Quick Test

### 30-Second Test

```bash
# Build
./build.sh build

# Run all tests
./build.sh test

# Expected output:
# ==========================================
# 55 Tests 0 Failures 0 Ignored
# OK
```

### 5-Minute Test

```bash
# Terminal 1: Start server
./build/matching_engine --tcp

# Terminal 2: Run scenarios
./build/tcp_client localhost 1234 1
./build/tcp_client localhost 1234 2
./build/tcp_client localhost 1234 3
```

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

Running test_AddSingleBuyOrder... PASS
Running test_AddSingleSellOrder... PASS
Running test_MatchingBuyAndSell... PASS
Running test_PartialFill... PASS
Running test_MultipleOrdersAtSamePrice... PASS
Running test_PriceTimePriority... PASS
Running test_CancelOrder... PASS
Running test_FlushOrderBook... PASS
Running test_TopOfBookUpdate... PASS
...

-----------------------
55 Tests 0 Failures 0 Ignored
OK
```

### Test Components

#### Order Book Tests (`test_order_book.c`)

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

#### Message Parser Tests (`test_message_parser.c`)

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

#### Scenario Tests (`test_scenarios_even.c`, `test_scenarios_odd.c`)

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

### Adding New Tests

1. Create test file in `tests/` directory:
```c
#include "unity.h"
#include "order_book.h"

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

2. Add to `tests/test_runner.c`:
```c
extern void test_YourTestName(void);

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_YourTestName);
    return UNITY_END();
}
```

3. Rebuild and run:
```bash
make test
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
```

Runs all unit tests with Unity framework.

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

---

## Performance Testing

### Throughput Test

```bash
# Generate high message volume
for i in {1..1000}; do
    echo "N, 1, IBM, 100, 50, B, $i"
done | nc localhost 1234
```

### Latency Test

Use the built-in statistics:

```bash
./build/matching_engine --tcp
# Run tests
# Ctrl+C to see statistics

=== Final Statistics ===
Processor:
  Messages processed: 10000
  Batches processed: 313
  Average batch size: 31.95
```

### Memory Test

```bash
# Run with valgrind
valgrind --leak-check=full ./build/matching_engine --tcp

# Or use make target
make valgrind-test
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

### Memory Leaks

```bash
# Run valgrind on tests
make valgrind-test

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
        run: sudo apt-get install build-essential cmake ninja-build
      - name: Build
        run: ./build.sh build
      - name: Test
        run: ./build.sh test
      - name: Valgrind
        run: ./build.sh valgrind
```

---

## Test Coverage Summary

| Category | Tests | Status |
|----------|-------|--------|
| Order Book | 15 | ✓ |
| Message Parser | 8 | ✓ |
| Message Formatter | 6 | ✓ |
| Matching Engine | 4 | ✓ |
| Binary Protocol | 12 | ✓ |
| Scenarios (1-16) | 16 | ✓ |
| TCP Integration | 1 | ✓ |
| **Total** | **62** | ✓ |

---

## Quick Reference

### Test Commands

```bash
./build.sh test           # Unit tests
./build.sh test-tcp       # TCP integration
./build.sh test-binary    # Binary protocol test
./build.sh valgrind       # Memory leak detection
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

---

## See Also

- [Quick Start Guide](documentation/QUICK_START.md) - Basic usage
- [Architecture](documentation/ARCHITECTURE.md) - System design
- [Protocols](documentation/PROTOCOLS.md) - Message formats
- [Build Instructions](documentation/BUILD.md) - Compiler setup
