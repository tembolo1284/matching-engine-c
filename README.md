# Matching Engine - C Implementation

A high-performance, multi-threaded order matching engine ported from C++ to pure C11. Implements price-time priority matching across multiple symbols with UDP input, dual protocol support (CSV/Binary), and real-time stdout output.

## Table of Contents
- [Overview](#overview)
- [Key Features](#key-features)
- [Architecture](#architecture)
- [Protocol Support](#protocol-support)
- [C Port Details](#c-port-details)
- [Building](#building)
- [Running](#running)
- [Testing](#testing)
- [Binary Protocol](#binary-protocol)
- [Project Structure](#project-structure)
- [Design Decisions](#design-decisions)
- [Performance Characteristics](#performance-characteristics)

---

## Overview

This is a **pure C port** of a C++ order matching engine originally designed for high-frequency trading environments. The system processes orders via UDP, maintains separate order books for multiple symbols, and publishes real-time market data to stdout. Now features **dual protocol support** with automatic format detection - accepts both CSV and binary protocols simultaneously for maximum flexibility.

## Key Features

### Core Functionality
- **Price-time priority matching** - Standard exchange algorithm
- **Multi-symbol support** - Independent order books per symbol
- **Market & limit orders** - Full and partial fill support
- **Order cancellation** - O(1) lookup via hash table
- **Flush command** - Clear all orders with cancel acknowledgements
- **Top-of-book tracking** - Real-time best bid/ask updates
- **Graceful shutdown** - Queue draining on exit

### Protocol Features
- **Dual protocol support** - CSV and binary protocols
- **Auto-detection** - Automatically detects message format
- **Mixed mode** - Accept both formats simultaneously
- **Backward compatible** - All existing CSV tests still work
- **Binary efficiency** - 50-70% smaller messages, 5-10x faster parsing

### Performance Optimizations
- **Lock-free queues** - SPSC queues with cache-line padding
- **Fixed-array price levels** - Binary search O(log N)
- **Batch processing** - 32 messages per iteration
- **UDP burst handling** - 10MB receive buffer
- **Zero-copy** - Minimal allocations in hot path
- **Adaptive sleep** - 1μs active, 100μs idle
- **Binary protocol** - Faster parsing, smaller packets

### Threading Model
```
Thread 1 (UDP Receiver) → Lock-Free Queue → Thread 2 (Processor) → Lock-Free Queue → Thread 3 (Output)
```

---

## Architecture

### Data Flow
```
UDP Packets (port 1234)
    ↓
[ Thread 1: UDP Receiver ]
    ↓ (auto-detect format)
    ├─→ CSV Parser → input_msg_t
    └─→ Binary Parser → input_msg_t
    ↓
[ Lock-Free Input Queue - 16384 capacity ]
    ↓
[ Thread 2: Processor ]
    ↓ (match orders → output_msg_t[])
[ Lock-Free Output Queue - 16384 capacity ]
    ↓
[ Thread 3: Output Publisher ]
    ↓ (format CSV or Binary)
stdout
```

### Core Components

**Data Structures:**
- `message_types.h` - Tagged unions for messages (replaces `std::variant`)
- `order.h` - Order struct with nanosecond timestamps
- `order_book.h` - Fixed-array price levels with binary search
- `matching_engine.h` - Multi-symbol orchestrator

**Protocol Handling:**
- `message_parser.h` - CSV input parser
- `message_formatter.h` - CSV output formatter
- `binary_message_parser.h` - Binary input parser
- `binary_message_formatter.h` - Binary output formatter
- `binary_protocol.h` - Binary message definitions

**Threading:**
- `udp_receiver.h` - Thread 1: POSIX UDP sockets with format detection
- `processor.h` - Thread 2: Batch processing with matching engine
- `output_publisher.h` - Thread 3: Format and publish to stdout
- `lockfree_queue.h` - SPSC queue implementation (C macro templates)

---

## Protocol Support

### CSV Protocol (Human-Readable)

**Input Format:**
```csv
N, userId, symbol, price, qty, side, userOrderId  # New order
C, userId, userOrderId                             # Cancel
F                                                  # Flush
```

**Output Format:**
```csv
A, symbol, userId, userOrderId                     # Acknowledgement
C, symbol, userId, userOrderId                     # Cancel ack
T, symbol, buyUser, buyOrder, sellUser, sellOrder, price, qty  # Trade
B, symbol, side, price, qty                        # Top of book
B, symbol, side, -, -                              # TOB eliminated
```

### Binary Protocol (High-Performance)

**Message Structure:**
- All messages start with magic byte `0x4D` ('M')
- Followed by message type byte ('N', 'C', 'F', 'A', 'X', 'T', 'B')
- Fixed-size structs with network byte order (big endian)
- 8-byte symbol field (null-padded)

**Message Sizes:**
```
New Order:       30 bytes  (vs ~40-50 for CSV)
Cancel:          11 bytes  (vs ~20-30 for CSV)
Flush:            2 bytes  (vs ~2 for CSV)
Acknowledgement: 19 bytes  (vs ~20-30 for CSV)
Trade:           31 bytes  (vs ~50-70 for CSV)
Top of Book:     20 bytes  (vs ~30-40 for CSV)
```

**Advantages:**
- 50-70% smaller packet size
- 5-10x faster parsing (memcpy vs string parsing)
- Deterministic performance (no variable-length fields)
- Network-efficient for high-frequency trading

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
# Clean build (includes binary tools)
make clean
make

# Build only main engine
make matching_engine

# Build only binary tools
make binary-tools

# Debug build (with symbols)
make debug

# View build configuration
make info
```

### Build Output
```
Compiling src/message_parser.c...
Compiling src/message_formatter.c...
Compiling src/binary_message_parser.c...
Compiling src/binary_message_formatter.c...
Compiling src/order_book.c...
Compiling src/matching_engine.c...
Compiling src/udp_receiver.c...
Compiling src/processor.c...
Compiling src/output_publisher.c...
Compiling src/main.c...
Linking build/matching_engine...
Building binary client...
Building binary decoder...

✓ Build complete!
  Main executable: build/matching_engine
  Binary tools:    build/binary_client, build/binary_decoder
```

---

## Running

### Basic Usage
```bash
# Run with default port (1234), CSV output
./build/matching_engine

# Run with binary output
./build/matching_engine --binary

# Run with binary output and live decoder
make run-binary-decoded

# Run with custom port
./build/matching_engine 5000

# Run with custom port and binary output
./build/matching_engine 5000 --binary
```

### Command Line Options
```
Usage: ./build/matching_engine [port] [--binary]
  port      : UDP port to listen on (default: 1234)
  --binary  : Use binary protocol for output (default: CSV)

Examples:
  ./build/matching_engine              # Port 1234, CSV output
  ./build/matching_engine 5000         # Port 5000, CSV output
  ./build/matching_engine --binary     # Port 1234, binary output
  ./build/matching_engine 5000 --binary # Port 5000, binary output
```

### Testing with CSV (Traditional)

**Terminal 1** - Start the matching engine:
```bash
./build/matching_engine 2> logs.txt | tee output.txt
```

**Terminal 2** - Send test orders:
```bash
# Send entire test file
cat data/inputFile.csv | nc -u localhost 1234

# Or send orders manually
echo "N, 1, IBM, 100, 50, B, 1" | nc -u localhost 1234
echo "N, 2, IBM, 100, 50, S, 2" | nc -u localhost 1234
echo "F" | nc -u localhost 1234
```

### Testing with Binary Protocol

**Send binary messages:**
```bash
# Terminal 1: Start server with CSV output (easier to read)
./build/matching_engine

# Terminal 2: Send binary test scenario
./build/binary_client 1234 1   # Simple orders
./build/binary_client 1234 2   # Trade scenario
./build/binary_client 1234 3   # Cancel scenario
```

**Full binary mode (binary input + binary output):**
```bash
# Terminal 1: Start with binary output and decoder
make run-binary-decoded

# Terminal 2: Send binary messages
./build/binary_client 1234 2
```

### Expected Output (CSV Format)
```
A, IBM, 1, 1
B, IBM, B, 100, 50
A, IBM, 2, 2
T, IBM, 1, 1, 2, 2, 100, 50
B, IBM, B, -, -
B, IBM, S, -, -
```

### Flush Command Output
When you send a flush command, all remaining orders are cancelled:
```
# Orders in book before flush
A, IBM, 1, 1
B, IBM, B, 100, 50
A, IBM, 2, 2
B, IBM, S, 105, 50

# Flush command sent
F

# Cancel acknowledgements for all orders
C, IBM, 1, 1
C, IBM, 2, 2
B, IBM, B, -, -
B, IBM, S, -, -
```

### Graceful Shutdown

Press `Ctrl+C` to initiate graceful shutdown:
```
^C
Received signal 2, shutting down gracefully...

=== Final Statistics ===
UDP Receiver:
  Packets received: 100
  Messages parsed:  100
  Messages dropped: 0

Processor:
  Messages processed: 100
  Batches processed:  4

Output Publisher:
  Messages published: 245

=== Matching Engine Stopped ===
```

---

## Testing

### Unity Testing Framework

We use [Unity](https://github.com/ThrowTheSwitch/Unity) - a lightweight C testing framework.

### Build and Run Tests
```bash
# Build and run unit tests
make test

# Test binary protocol (CSV output mode)
make test-binary

# Test full binary protocol (binary in/out)
make test-binary-full

# Run all tests
make test-all

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
Running test_FlushOrderBook... PASS
Running test_Scenario1_BalancedBook... PASS
Running test_Scenario2_ShallowBid... PASS
...

-----------------------
55 Tests 0 Failures 0 Ignored
OK
```

### Binary Protocol Tests
```bash
# Automated binary protocol test
make test-binary
```

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
B, IBM, B, -, -
B, IBM, S, -, -

✓ Binary protocol test complete
```

### Test Coverage

**Component Tests:**
- `test_message_parser.c` - CSV parsing validation
- `test_message_formatter.c` - Output formatting
- `test_order_book.c` - Matching logic, price-time priority, flush
- `test_matching_engine.c` - Multi-symbol routing

**Scenario Tests:**
- `test_scenarios_odd.c` - Scenarios 1, 3, 5, 7, 9, 11, 13, 15
- `test_scenarios_even.c` - Scenarios 2, 4, 6, 8, 10, 12, 14, 16
- All scenarios include flush command validation

**Binary Protocol Tests:**
- Automated testing via `make test-binary`
- Manual testing via binary client tool
- Format detection validation

---

## Binary Protocol

### Binary Tools

**Binary Client** (`build/binary_client`)
```bash
# Usage
./build/binary_client <port> [scenario]

# Scenarios:
#   1 - Simple orders (buy + sell + flush)
#   2 - Trade scenario (matching orders)
#   3 - Cancel scenario (order cancellation)

# Examples
./build/binary_client 1234 1   # Simple test
./build/binary_client 1234 2   # Trade test
./build/binary_client 1234 3   # Cancel test
```

**Binary Decoder** (`build/binary_decoder`)
```bash
# Decode binary output to human-readable CSV
./build/matching_engine --binary 2>&1 | ./build/binary_decoder
```

### Creating Custom Binary Messages

Example in C:
```c
#include <arpa/inet.h>

typedef struct __attribute__((packed)) {
    uint8_t  magic;         // 0x4D
    uint8_t  msg_type;      // 'N'
    uint32_t user_id;       // htonl(1)
    char     symbol[8];     // "IBM\0\0\0\0\0"
    uint32_t price;         // htonl(100)
    uint32_t quantity;      // htonl(50)
    uint8_t  side;          // 'B'
    uint32_t user_order_id; // htonl(1)
} binary_new_order_t;

// Send via UDP
sendto(sock, &msg, sizeof(msg), 0, ...);
```

### Binary Format Specification

**Magic Byte Detection:**
```c
// First byte = 0x4D indicates binary protocol
if (data[0] == 0x4D) {
    // Binary message
} else {
    // CSV message
}
```

**Message Types:**
- `'N'` - New Order (30 bytes)
- `'C'` - Cancel (11 bytes)
- `'F'` - Flush (2 bytes)
- `'A'` - Acknowledgement (19 bytes)
- `'X'` - Cancel Ack (19 bytes)
- `'T'` - Trade (31 bytes)
- `'B'` - Top of Book (20 bytes)

**All integers are in network byte order (big endian)** - use `htonl()` to convert.

---

# TCP Multi-Client Quick Start

## Building
```bash
make clean
make
```

This builds:
- `build/matching_engine` - Main server
- `build/tcp_client` - TCP test client
- `build/binary_client` - UDP binary test client
- `build/binary_decoder` - Binary protocol decoder

## Running the TCP Server

### Basic TCP Server (CSV output)
```bash
./build/matching_engine --tcp
```

### TCP Server on Custom Port
```bash
./build/matching_engine --tcp 5000
```

### TCP Server with Binary Output
```bash
./build/matching_engine --tcp --binary
```

## Testing with TCP Client

### Terminal 1: Start Server
```bash
./build/matching_engine --tcp 1234
```

### Terminal 2: Run Test Scenarios

**Scenario 1 - Simple Orders:**
```bash
./build/tcp_client localhost 1234 1
```

**Scenario 2 - Matching Trade:**
```bash
./build/tcp_client localhost 1234 2
```

**Scenario 3 - Cancel Order:**
```bash
./build/tcp_client localhost 1234 3
```

**Interactive Mode:**
```bash
./build/tcp_client localhost 1234
```

Then type orders:
```
> N, 1, IBM, 100, 50, B, 1
> N, 1, IBM, 100, 50, S, 2
> F
> quit
```

## Multiple Clients

Each TCP client gets a unique `client_id` (1-based). The server validates that the `userId` in orders matches the assigned `client_id`.

**Terminal 1: Server**
```bash
./build/matching_engine --tcp
```

**Terminal 2: Client 1 (will be assigned client_id=1)**
```bash
./build/tcp_client localhost 1234
> N, 1, IBM, 100, 50, B, 1
```

**Terminal 3: Client 2 (will be assigned client_id=2)**
```bash
./build/tcp_client localhost 1234
> N, 2, IBM, 100, 50, S, 1
```

When Client 2's sell order matches Client 1's buy order, **both clients receive the trade message**.

## Expected Output

**Client 1 sends buy order:**
```
> N, 1, IBM, 100, 50, B, 1

Server responds:
A, IBM, 1, 1
B, IBM, B, 100, 50
```

**Client 2 sends matching sell order:**
```
> N, 2, IBM, 100, 50, S, 1

Server responds to Client 2:
A, IBM, 2, 1
T, IBM, 1, 1, 2, 1, 100, 50
B, IBM, B, -, -
B, IBM, S, -, -

Server responds to Client 1:
T, IBM, 1, 1, 2, 1, 100, 50
B, IBM, B, -, -
B, IBM, S, -, -
```

Both clients see the trade!

## Running Tests
```bash
# Unit tests
make test

# TCP integration test
make test-tcp

# All tests
make test-all
```

## Legacy UDP Mode

The old UDP mode still works:
```bash
# UDP mode
./build/matching_engine --udp 1234

# Send via netcat
echo "N, 1, IBM, 100, 50, B, 1" | nc -u localhost 1234
```

## Protocol Details

### Framing (TCP)
All messages are length-prefixed:
```
[4-byte length (big-endian)][message payload]
```

### Message Format (CSV)
```
New Order:  N, userId, symbol, price, qty, side, userOrderId
Cancel:     C, userId, userOrderId
Flush:      F
```

### Security
- Each TCP client is assigned a unique `client_id` on connection
- Server validates that `userId` in messages matches the client's assigned `client_id`
- Prevents clients from spoofing other clients' orders
- On disconnect, all client's orders are automatically cancelled

## Architecture
```
TCP Client 1 ──┐
TCP Client 2 ──┼──> TCP Listener ──> Input Queue ──> Processor ──> Output Router
TCP Client 3 ──┘         ↑                                              │
                         │                                              │
                         └──────────────────────────────────────────────┘
                              (per-client output queues)
```

- **TCP Listener**: epoll-based event loop, handles all client I/O
- **Processor**: Matching engine, generates output with client routing info
- **Output Router**: Routes messages to appropriate client queues
- **Lock-free queues**: Zero contention between threads

## Project Structure
```
matching-engine-c/
├── Makefile                    # Build system
├── README.md                   # This file
│
├── include/                    # Header files (13 files)
│   ├── message_types.h         # Core types (tagged unions)
│   ├── order.h                 # Order structure
│   ├── order_book.h            # Price level management
│   ├── matching_engine.h       # Multi-symbol orchestrator
│   ├── message_parser.h        # CSV input parser
│   ├── message_formatter.h     # CSV output formatter
│   ├── binary_protocol.h       # Binary message definitions
│   ├── binary_message_parser.h # Binary input parser
│   ├── binary_message_formatter.h # Binary output formatter
│   ├── lockfree_queue.h        # SPSC queue (macros)
│   ├── udp_receiver.h          # UDP receiver thread
│   ├── processor.h             # Processor thread
│   └── output_publisher.h      # Output thread
│
├── src/                        # Implementation files (10 files)
│   ├── main.c                  # Entry point, thread orchestration
│   ├── order_book.c            # Core matching logic (600+ lines)
│   ├── matching_engine.c       # Symbol routing
│   ├── message_parser.c        # CSV parsing
│   ├── message_formatter.c     # CSV formatting
│   ├── binary_message_parser.c # Binary parsing
│   ├── binary_message_formatter.c # Binary formatting
│   ├── udp_receiver.c          # POSIX UDP sockets + auto-detection
│   ├── processor.c             # Batch processing
│   └── output_publisher.c      # Output loop (CSV or binary)
│
├── tools/                      # Binary protocol tools
│   ├── binary_client.c         # Binary message sender
│   └── binary_decoder.c        # Binary to CSV decoder
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
│   ├── test_scenarios_odd.c    # Odd scenario validation
│   └── test_scenarios_even.c   # Even scenario validation
│
├── data/                       # Test data
│   ├── inputFile.csv           # Test input
│   └── output_file.csv         # Expected output
│
└── build/                      # Build artifacts (generated)
    ├── matching_engine         # Main executable
    ├── matching_engine_tests   # Test executable
    ├── binary_client           # Binary test client
    ├── binary_decoder          # Binary decoder
    └── obj/                    # Object files
        ├── *.o                 # Main object files
        └── tests/              # Test object files
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

### 4. **Dual Protocol Support**

**Choice:** Auto-detection with magic byte

**Rationale:**
- Zero configuration required
- Backward compatible with all CSV tests
- Performance boost for binary clients
- Mixed-mode operation for gradual migration

### 5. **Macros vs Function Pointers for "Templates"**

**Choice:** C macros for lock-free queue

**Rationale:**
- Zero runtime overhead
- Type safety at compile time
- Similar to C++ templates
- Explicit code generation

### 6. **pthread vs C11 threads**

**Choice:** POSIX pthreads

**Rationale:**
- Broader platform support
- More mature ecosystem
- Better debugging tools
- Industry standard for systems programming

### 7. **Flush Command Enhancement**

**Choice:** Generate cancel acknowledgements for all orders

**Rationale:**
- Visibility into what was cancelled
- Consistent with individual cancel behavior
- Helps with audit trails and reconciliation
- Minimal performance impact (flush is rare)

---

## Performance Characteristics

### Throughput
- **UDP Receiver**: ~1-10M packets/sec (network limited)
- **Matching Engine**: ~1-5M orders/sec (single-threaded)
- **Output Publisher**: ~500K-1M messages/sec (stdout limited)

### Latency
- **Queue hop**: ~100-500ns (lock-free)
- **CSV parsing**: ~500-2000ns per message
- **Binary parsing**: ~50-200ns per message (5-10x faster)
- **Matching**: ~1-10μs (depends on book depth)
- **End-to-end**: ~10-50μs (UDP → match → stdout)

### Memory
- **Fixed queues**: 2 × 16,384 × sizeof(msg) ≈ 2-4MB
- **Order books**: Dynamic (grows with orders)
- **Typical usage**: 10-100MB
- **Binary messages**: 50-70% smaller than CSV

### Protocol Comparison

| Metric | CSV | Binary | Improvement |
|--------|-----|--------|-------------|
| New Order Size | ~40-50 bytes | 30 bytes | 40-60% smaller |
| Parsing Time | ~500-2000ns | ~50-200ns | 5-10x faster |
| Network Bandwidth | Baseline | 50-70% less | 2x more efficient |
| Human Readable | ✓ | ✗ | Trade-off |
| Debugging | Easy | Requires decoder | Trade-off |

---

## Future Enhancements

### Potential Improvements
- **TCP support** - For guaranteed delivery
- **Multi-client mode** - Handle multiple connections
- **Market data snapshots** - Full book state queries
- **Historical replay** - Replay past messages
- **Performance counters** - Detailed latency histograms
- **Configuration file** - Runtime configuration
- **Level 2 market data** - Multiple price levels

### Advanced Features
- **Order types** - Stop-loss, iceberg, time-in-force
- **Risk limits** - Position limits, rate limiting
- **Market making** - Special order handling
- **Cross orders** - Internal matching
- **Auction mode** - Opening/closing auctions

