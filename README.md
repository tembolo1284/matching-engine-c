# Matching Engine - C Implementation

A high-performance, multi-threaded order matching engine ported from C++ to pure C11. Implements price-time priority matching across multiple symbols with UDP input and real-time stdout output.

## Table of Contents
- [Overview](#overview)
- [Key Features](#key-features)
- [Architecture](#architecture)
- [C Port Details](#c-port-details)
- [Building](#building)
- [Running](#running)
- [Testing](#testing)
- [Project Structure](#project-structure)
- [Design Decisions](#design-decisions)

---

## Overview

This is a **pure C port** of a C++ order matching engine originally designed for high-frequency trading environments. The system processes orders via UDP, maintains separate order books for multiple symbols, and publishes real-time market data to stdout.

## Key Features

### Core Functionality
- **Price-time priority matching** - Standard exchange algorithm
- **Multi-symbol support** - Independent order books per symbol
- **Market & limit orders** - Full and partial fill support
- **Order cancellation** - O(1) lookup via hash table
- **Top-of-book tracking** - Real-time best bid/ask updates
- **Graceful shutdown** - Queue draining on exit

### Performance Optimizations
- **Lock-free queues** - SPSC queues with cache-line padding
- **Fixed-array price levels** - Binary search O(log N)
- **Batch processing** - 32 messages per iteration
- **UDP burst handling** - 10MB receive buffer
- **Zero-copy** - Minimal allocations in hot path
- **Adaptive sleep** - 1μs active, 100μs idle

### Threading Model
```
Thread 1 (UDP Receiver) → Lock-Free Queue → Thread 2 (Processor) → Lock-Free Queue → Thread 3 (Output)
```
## Architecture

### Data Flow
```
UDP Packets (port 1234)
    ↓
[ Thread 1: UDP Receiver ]
    ↓ (parse CSV → input_msg_t)
[ Lock-Free Input Queue - 16384 capacity ]
    ↓
[ Thread 2: Processor ]
    ↓ (match orders → output_msg_t[])
[ Lock-Free Output Queue - 16384 capacity ]
    ↓
[ Thread 3: Output Publisher ]
    ↓ (format CSV)
stdout
```

### Core Components

**Data Structures:**
- `message_types.h` - Tagged unions for messages (replaces `std::variant`)
- `order.h` - Order struct with nanosecond timestamps
- `order_book.h` - Fixed-array price levels with binary search
- `matching_engine.h` - Multi-symbol orchestrator

**Threading:**
- `udp_receiver.h` - Thread 1: POSIX UDP sockets
- `processor.h` - Thread 2: Batch processing with matching engine
- `output_publisher.h` - Thread 3: Format and publish to stdout
- `lockfree_queue.h` - SPSC queue implementation (C macro templates)

---

## C Port Details

### How We Replaced C++ Features

| C++ Feature | C Replacement | Implementation |
|-------------|---------------|----------------|
| `std::variant` | Tagged unions | `typedef struct { type_t type; union { ... } data; }` |
| `std::optional` | Bool + output param | `bool func(..., output_t* out)` |
| `std::string` | Fixed char arrays | `char symbol[MAX_SYMBOL_LENGTH]` |
| `std::map` | Fixed array + binary search | `price_level_t levels[10000]` |
| `std::list` | Manual doubly-linked list | `order_t* next; order_t* prev;` |
| `std::unordered_map` | Hash table with chaining | Custom implementation |
| `std::vector` | Fixed array + counter | `output_msg_t messages[1024]; int count;` |
| `std::thread` | pthreads | `pthread_create()`, `pthread_join()` |
| `std::atomic` | C11 `<stdatomic.h>` | `atomic_bool`, `atomic_uint_fast64_t` |
| `boost::asio` | POSIX sockets | `socket()`, `recvfrom()`, `bind()` |
| `std::chrono` | `<time.h>` | `clock_gettime(CLOCK_REALTIME)` |
| Templates | Macros | `DECLARE_LOCKFREE_QUEUE(type, name)` |

---

## Building

### Prerequisites
```bash
# Ubuntu/Debian
sudo apt-get install build-essential

# macOS
xcode-select --install

# Required: GCC with C11 support
gcc --version  # Should be 4.9+
```

### Quick Build
```bash
# Clean build
make clean
make

# Debug build (with symbols)
make debug

# View build configuration
make info
```

### Build Output
```
Compiling src/message_parser.c...
Compiling src/message_formatter.c...
Compiling src/order_book.c...
Compiling src/matching_engine.c...
Compiling src/udp_receiver.c...
Compiling src/processor.c...
Compiling src/output_publisher.c...
Compiling src/main.c...
Linking ./matching_engine...
✓ Build complete: ./matching_engine
```

---

## Running

### Basic Usage
```bash
# Run with default port (1234)
./matching_engine

# Run with custom port
./matching_engine 5000
```

### Testing with netcat

**Terminal 1** - Start the matching engine:
```bash
./matching_engine 2> logs.txt | tee output.txt
```

**Terminal 2** - Send test orders:
```bash
# Send entire test file
cat data/inputFile.csv | nc -u localhost 1234

# Or send orders manually
echo "N, 1, IBM, 100, 50, B, 1" | nc -u localhost 1234
echo "N, 2, IBM, 100, 50, S, 2" | nc -u localhost 1234
```

**Terminal 3** - Monitor logs:
```bash
tail -f logs.txt
```

### Expected Output
```
A, 1, 1
B, B, 100, 50
A, 2, 2
T, 1, 1, 2, 2, 100, 50
B, B, -, -
B, S, -, -
```

### Graceful Shutdown

Press `Ctrl+C` to initiate graceful shutdown:
```
^C
Shutdown signal received...
==============================================================
Initiating graceful shutdown...
Stopping UDP receiver...
Draining input queue (size: 0)...
Stopping processor...
Draining output queue (size: 0)...
Stopping output publisher...
==============================================================
Final Statistics:
  Packets received:    100
  Messages parsed:     100
  Messages processed:  100
  Batches processed:   4
  Messages published:  245
==============================================================
Shutdown complete. Goodbye!
```

---

## Testing

### Unity Testing Framework

We use [Unity](https://github.com/ThrowTheSwitch/Unity) - a lightweight C testing framework designed for embedded systems.

### Build and Run Tests
```bash
# Build and run all tests
make test

# Run tests with valgrind
make valgrind-test
```

### Test Output
```
==========================================
Running Unity Tests
==========================================

Running test_AddSingleBuyOrder... PASS
Running test_AddSingleSellOrder... PASS
Running test_MatchingBuyAndSell... PASS
Running test_PartialFill... PASS
Running test_MarketOrderBuy... PASS
Running test_PriceTimePriority... PASS
...

-----------------------
47 Tests 0 Failures 0 Ignored
OK
```

### Test Coverage

**Component Tests:**
- `test_message_parser.c` - CSV parsing validation
- `test_message_formatter.c` - Output formatting
- `test_order_book.c` - Matching logic, price-time priority
- `test_matching_engine.c` - Multi-symbol routing

**Scenario Tests:**
- `test_scenarios.c` - Validates against provided test scenarios
- All 16 scenarios from original C++ tests

### Adding New Tests

1. Create test file in `tests/`
2. Include Unity: `#include "unity.h"`
3. Write test functions: `void test_MyFeature(void) { TEST_ASSERT_EQUAL(...); }`
4. Add to `test_runner.c`: `RUN_TEST(test_MyFeature);`
5. Rebuild: `make test`

---

## Project Structure
```
kraken-c/
├── Makefile                    # Build system
├── README.md                   # This file
│
├── include/                    # Header files (10 files)
│   ├── message_types.h         # Core types (tagged unions)
│   ├── order.h                 # Order structure
│   ├── order_book.h            # Price level management
│   ├── matching_engine.h       # Multi-symbol orchestrator
│   ├── message_parser.h        # CSV input parser
│   ├── message_formatter.h     # CSV output formatter
│   ├── lockfree_queue.h        # SPSC queue (macros)
│   ├── udp_receiver.h          # UDP receiver thread
│   ├── processor.h             # Processor thread
│   └── output_publisher.h      # Output thread
│
├── src/                        # Implementation files (8 files)
│   ├── main.c                  # Entry point, thread orchestration
│   ├── order_book.c            # Core matching logic (500+ lines)
│   ├── matching_engine.c       # Symbol routing
│   ├── message_parser.c        # CSV parsing
│   ├── message_formatter.c     # CSV formatting
│   ├── udp_receiver.c          # POSIX UDP sockets
│   ├── processor.c             # Batch processing
│   └── output_publisher.c      # Output loop
│
├── tests/                      # Unity test framework
│   ├── unity.h                 # Unity header
│   ├── unity.c                 # Unity implementation
│   ├── unity_internals.h       # Unity internals
│   ├── test_runner.c           # Test main()
│   ├── test_order_book.c       # Order book tests
│   ├── test_message_parser.c   # Parser tests
│   ├── test_message_formatter.c# Formatter tests
│   ├── test_matching_engine.c  # Engine tests
│   └── test_scenarios.c        # Scenario validation
│
├── data/                       # Test data
│   ├── inputFile.csv           # Test input
│   └── output_file.csv         # Expected output
│
└── obj/                        # Build artifacts (generated)
    ├── *.o                     # Object files
    └── tests/                  # Test object files
```

---

## Design Decisions

### 1. **Fixed Arrays vs Dynamic Allocation**

**Choice:** Fixed-size arrays for price levels (10,000 slots)

**Rationale:**
- Predictable memory usage
- Better cache locality
- No malloc/free in matching hot path
- Typical order books have ~100 active price levels

### 2. **Binary Search vs Hash Table for Prices**

**Choice:** Binary search on sorted array

**Rationale:**
- O(log N) lookup is fast for N < 10,000
- Maintains sorted order naturally
- Better cache performance than tree structures
- Simpler than red-black trees

### 3. **Manual Lists vs Library**

**Choice:** Manual doubly-linked lists for orders

**Rationale:**
- Full control over memory layout
- No external dependencies
- O(1) deletion with iterator
- Simple implementation

### 4. **Macros vs Function Pointers for "Templates"**

**Choice:** C macros for lock-free queue

**Rationale:**
- Zero runtime overhead
- Type safety at compile time
- Similar to C++ templates
- Explicit code generation

### 5. **pthread vs C11 threads**

**Choice:** POSIX pthreads

---

## Performance Characteristics

### Throughput
- **UDP Receiver**: ~1-10M packets/sec (network limited)
- **Matching Engine**: ~1-5M orders/sec (single-threaded)
- **Output Publisher**: ~500K-1M messages/sec (stdout limited)

### Latency
- **Queue hop**: ~100-500ns (lock-free)
- **Matching**: ~1-10μs (depends on book depth)
- **End-to-end**: ~10-50μs (UDP → match → stdout)

### Memory
- **Fixed queues**: 2 × 16,384 × sizeof(msg) ≈ 2-4MB
- **Order books**: Dynamic (grows with orders)
- **Typical usage**: 10-100MB

---

## Future Enhancements

### Advanced:
- Symbol-based sharding (horizontal scaling)
- Lock-free order book (eliminate all locks)
- Binary wire protocol
- Kernel bypass (DPDK, io_uring)
- NUMA-aware memory allocation

