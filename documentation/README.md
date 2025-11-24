# Matching Engine - C Implementation

A high-performance, multi-threaded order matching engine written in pure C11. Implements price-time priority matching across multiple symbols with dual protocol support (CSV/Binary), TCP multi-client support, and real-time output.

## Overview

This is a **pure C port** of a C++ order matching engine originally designed for high-frequency trading environments. The system processes orders via UDP or TCP, maintains separate order books for multiple symbols, and publishes real-time market data.

## Key Features

- **Price-time priority matching** - Standard exchange algorithm
- **Multi-symbol support** - Independent order books per symbol
- **Dual protocol support** - CSV (human-readable) and Binary (high-performance)
- **TCP multi-client** - Multiple simultaneous connections with per-client isolation
- **UDP mode** - High-throughput fire-and-forget messaging
- **Lock-free queues** - SPSC queues for zero-contention threading
- **Binary protocol** - 50-70% smaller messages, 5-10x faster parsing
- **Auto-detection** - Automatically detects CSV vs Binary format

## Quick Links

- **[Quick Start Guide](QUICK_START.md)** - Get up and running in 5 minutes
- **[Architecture](ARCHITECTURE.md)** - System design and components
- **[Protocols](PROTOCOLS.md)** - CSV and Binary protocol specifications
- **[Testing](TESTING.md)** - Comprehensive testing guide
- **[Build Instructions](BUILD.md)** - Detailed build and prerequisites

## Quick Start

```bash
# Build the project
make clean && make

# Start TCP server
./build/matching_engine --tcp

# In another terminal, run test client
./build/tcp_client localhost 1234 2
```

See [QUICK_START.md](QUICK_START.md) for more details.

## Threading Model

```
TCP/UDP Input → Lock-Free Queue → Processor → Output Router → Client Queues
  (Thread 1)                       (Thread 2)      (Thread 3)
```

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed architecture.

## Protocols

### CSV Protocol (Human-Readable)
```csv
N, userId, symbol, price, qty, side, userOrderId  # New order
C, userId, userOrderId                             # Cancel
F                                                  # Flush
```

### Binary Protocol (High-Performance)
- All messages start with magic byte `0x4D`
- Fixed-size structs with network byte order
- 50-70% smaller than CSV, 5-10x faster parsing

See [PROTOCOLS.md](PROTOCOLS.md) for complete protocol documentation.

## Project Structure

```
matching-engine-c/
├── README.md           # This file
├── QUICK_START.md      # Getting started guide
├── ARCHITECTURE.md     # System design
├── PROTOCOLS.md        # Protocol specifications
├── TESTING.md          # Testing guide
├── BUILD.md            # Build instructions
├── Makefile            # Build system
├── include/            # Header files
├── src/                # Implementation
├── tools/              # Binary protocol tools
├── tests/              # Unity test framework
└── build/              # Build artifacts
```

## C Port Details

This project demonstrates how to port C++ features to pure C11:

| C++ Feature | C Replacement |
|-------------|---------------|
| `std::variant` | Tagged unions |
| `std::optional` | Bool + output param |
| `std::string` | Fixed char arrays |
| `std::map` | Binary search on sorted array |
| `std::unordered_map` | Custom hash table |
| `std::thread` | pthreads |
| `std::atomic` | C11 `<stdatomic.h>` |
| Templates | C macros |

## Performance

- **Throughput**: 1-5M orders/sec (matching engine)
- **Latency**: 10-50μs end-to-end (UDP → match → output)
- **Binary parsing**: 5-10x faster than CSV
- **Memory**: 10-100MB typical usage

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed performance characteristics.

## Testing

```bash
# Run all tests
make test-all

# TCP integration test
make test-tcp

# Binary protocol test
make test-binary
```

See [TESTING.md](TESTING.md) for comprehensive testing guide.

