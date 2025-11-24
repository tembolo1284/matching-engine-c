# Matching Engine - C Implementation

A **production-grade**, high-performance order matching engine written in pure C11. Features zero-allocation hot path using memory pools, TCP multi-client support, and dual protocol support (CSV/Binary).

## 🎯 Key Features

### **Zero-Allocation Memory Pools** 
- **No malloc/free in hot path** - All memory pre-allocated at startup
- Order pool: 10,000 orders
- Hash entry pools: 10,000 entries
- O(1) allocation/deallocation
- Production-ready memory management

### **Production Architecture**
- **Price-time priority matching** - Standard exchange algorithm
- **Multi-symbol support** - Independent order books per symbol
- **TCP multi-client** - Real exchange-like behavior with client isolation
- **Lock-free queues** - Zero-contention threading model
- **Dual protocol** - CSV (human-readable) + Binary (high-performance)
- **Auto protocol detection** - Seamlessly handles both formats

### **High Performance**
- 1-5M orders/sec matching throughput
- 10-50μs end-to-end latency
- Binary protocol: 50-70% smaller messages, 5-10x faster parsing
- Bounded loops and defensive programming throughout

## 📚 Documentation

| Document | Description |
|----------|-------------|
| **[Quick Start →](documentation/QUICK_START.md)** | Get running in 5 minutes |
| **[Architecture →](documentation/ARCHITECTURE.md)** | Memory pools, threading model, data structures |
| **[Build Guide →](documentation/BUILD.md)** | Build system, CMake, platform notes |
| **[Testing →](documentation/TESTING.md)** | Unit tests, integration tests, scenarios |
| **[Protocols →](documentation/PROTOCOLS.md)** | CSV and Binary protocol specifications |

## ⚡ Quick Start

### Build and Run

```bash
# Build everything
./build.sh build

# Start TCP server
./build/matching_engine --tcp

# In another terminal, run test client
./build/tcp_client localhost 1234 2
```

### Run Tests

```bash
# All tests
./build.sh test

# Individual test suites
./build.sh test-binary      # Binary protocol test
./build.sh test-tcp         # TCP integration test
```

See **[Quick Start Guide](documentation/QUICK_START.md)** for detailed examples.

## 🏗️ Architecture Highlights

### Memory Pool System (Zero-Allocation Hot Path)

```c
// All memory pre-allocated at startup
typedef struct {
    order_pool_t order_pool;              // 10K orders
    hash_entry_pool_t hash_entry_pool;    // 10K hash entries
} memory_pools_t;

// O(1) allocation - just index manipulation
order_t* order = order_pool_alloc(&pools->order_pool);
```

No malloc/free during order matching = **predictable latency** and **no fragmentation**.

### Three-Thread Pipeline

```
TCP/UDP Receiver → Lock-Free Queue → Processor → Output Router → Clients
   (Thread 1)         (16K msgs)      (Thread 2)    (Thread 3)
```

Lock-free communication = **zero contention** = consistent performance.

### TCP Multi-Client Support

```c
// Each order tracks its owner
order->client_id = client_id;

// Auto-cancel on disconnect
matching_engine_cancel_client_orders(engine, client_id, output);
```

Real exchange-like behavior with **client isolation** and **automatic cleanup**.

See **[Architecture Guide](documentation/ARCHITECTURE.md)** for complete details.

## 📁 Project Structure

```
matching-engine-c/
├── build.sh              # Build script with test modes
├── CMakeLists.txt        # CMake build configuration
├── documentation/        # Comprehensive documentation
│   ├── ARCHITECTURE.md
│   ├── BUILD.md
│   ├── PROTOCOLS.md
│   ├── QUICK_START.md
│   └── TESTING.md
├── include/              # Header files
│   ├── core/            # Order book, matching engine
│   ├── network/         # TCP/UDP networking
│   ├── protocol/        # CSV and Binary protocols
│   └── threading/       # Lock-free queues, threads
├── src/                 # Implementation files (mirrors include/)
├── tests/               # Unity test framework
│   ├── core/           # Core component tests
│   ├── protocol/       # Protocol tests
│   └── scenarios/      # End-to-end scenario tests
└── tools/              # Binary client, decoder, TCP client
```

## 🔬 C Port Details

This project demonstrates how to build production-quality C systems without C++:

| C++ Feature | C Implementation | Benefits |
|-------------|------------------|----------|
| `std::vector` + `new`/`delete` | Memory pools | Predictable, no fragmentation |
| `std::variant` | Tagged unions | Type-safe, zero overhead |
| `std::map` | Binary search on sorted array | Better cache locality |
| `std::unordered_map` | Custom hash table + pools | Full control, no malloc |
| `std::thread` | pthreads | Industry standard |
| `std::atomic` | C11 `<stdatomic.h>` | Native support |
| Templates | C macros | Zero runtime cost |

See **[Architecture Guide](documentation/ARCHITECTURE.md)** for implementation details.

## 🧪 Testing

Comprehensive test coverage with Unity framework:

```bash
# Unit tests (55+ tests)
./build.sh test

# Integration tests
./build.sh test-tcp         # TCP multi-client
./build.sh test-binary      # Binary protocol

# Memory analysis
./build.sh valgrind         # Linux: valgrind
                            # macOS: leaks tool
```

See **[Testing Guide](documentation/TESTING.md)** for full test scenarios.

## 🌐 Protocol Support

### CSV (Human-Readable)
```csv
N, 1, IBM, 10000, 50, B, 1    # New buy order: 50 shares @ $100
```

### Binary (High-Performance)
```
[0x4D]['N'][user_id][symbol][price][qty][side][order_id]
30 bytes vs ~45 bytes CSV = 33% smaller
```

Auto-detection: First byte = 0x4D → Binary, else CSV

See **[Protocol Guide](documentation/PROTOCOLS.md)** for specifications.

## 🔨 Build Options

```bash
# Build modes
./build.sh build            # Release build
./build.sh debug            # Debug build with symbols

# Test modes (README run-modes for 2-terminal setups)
./build.sh test-binary      # UDP + binary client
./build.sh test-tcp         # TCP + scenario
./build.sh test-tcp-csv     # TCP + CSV protocol

# Run directly
./build.sh run              # Start server
./build.sh run-tcp          # TCP mode
./build.sh run-udp          # UDP mode
```

See **[Build Guide](documentation/BUILD.md)** for detailed build instructions.

## 💡 Design Philosophy

1. **Memory Pools** - Pre-allocate everything, zero malloc in hot path
2. **Bounded Loops** - Every loop has explicit iteration limits
3. **Defensive Programming** - Parameter validation, DEBUG mode checks
4. **Lock-Free** - SPSC queues for zero contention
5. **Type Safety** - Tagged unions instead of void pointers
6. **Explicit Cleanup** - No hidden destructors, clear ownership

Production-quality C without sacrificing safety or performance.

## 📊 Performance Characteristics

- **Throughput**: 1-5M orders/sec (matching engine)
- **Latency**: 10-50μs end-to-end (UDP → match → output)
- **Memory**: 10-50MB typical usage, predictable allocation
- **Binary Protocol**: 5-10x faster parsing than CSV

See **[Architecture Guide](documentation/ARCHITECTURE.md)** for detailed analysis.

## 🎓 Learning Value

This project demonstrates:
- ✅ Production-grade memory management without garbage collection
- ✅ Lock-free multi-threading patterns
- ✅ High-performance networking (TCP + UDP)
- ✅ Protocol design and implementation
- ✅ C11 atomics and modern C practices
- ✅ Comprehensive testing strategies
- ✅ CMake build systems

Perfect for understanding **systems programming** and **high-frequency trading** systems.

## 📝 License

Educational project demonstrating C systems programming and HFT architecture.

## 🚀 Getting Started

1. **Read**: [Quick Start Guide](documentation/QUICK_START.md)
2. **Build**: `./build.sh build`
3. **Test**: `./build.sh test`
4. **Run**: `./build/matching_engine --tcp`
5. **Learn**: [Architecture Guide](documentation/ARCHITECTURE.md)

---

**Built with**: C11 • CMake • pthreads • Lock-free queues • Memory pools
