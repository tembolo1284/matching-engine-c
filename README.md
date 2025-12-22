# High-Performance Matching Engine in C

A production-grade, cache-optimized order matching engine designed for high-frequency trading (HFT) applications. Built in C11 with a focus on sub-microsecond latency, deterministic performance, and safety-critical coding standards.

## Key Features

### Core Matching Engine
- **Price-Time Priority**: Orders matched by best price, then earliest timestamp (FIFO)
- **Order Types**: Limit orders and market orders (price = 0)
- **Partial Fills**: Large orders match against multiple resting orders
- **Multi-Symbol Support**: Independent order books per trading symbol
- **Order Cancellation**: Cancel individual orders or flush entire books

### High-Performance Architecture
- **Zero-Allocation Hot Path**: Memory pools pre-allocate all structures at startup
- **Cache-Line Optimized**: All hot structures aligned to 64-byte boundaries
- **Open-Addressing Hash Tables**: Linear probing for cache-friendly O(1) lookups
- **Lock-Free Queues**: SPSC queues with false-sharing prevention and batch operations
- **RDTSC Timestamps**: Serialized `rdtscp` for ~5 cycle timestamps on x86-64
- **Packed Enums**: `uint8_t` enums save 3 bytes per field vs standard enums
- **Batch Dequeue**: Amortizes atomic operations across up to 32 messages

### Network Layer
- **UDP Mode**: High-throughput single-client with multicast market data
- **TCP Mode**: Multi-client support with per-client message routing
- **Dual-Processor Mode**: Horizontal scaling with symbol-based partitioning (A-M / N-Z)
- **Low-Latency Sockets**: `TCP_NODELAY`, `TCP_QUICKACK`, `SO_BUSY_POLL` optimizations
- **DPDK Kernel Bypass**: Optional ~25x latency reduction for UDP/multicast (Linux)

### Protocol Support
- **CSV Protocol**: Human-readable format for debugging and testing
- **Binary Protocol**: Packed structs with compile-time size verification
- **Auto-Detection**: Per-client protocol detection (binary vs CSV)
- **Branchless Routing**: Symbol-based routing with zero conditional branches

### Safety & Reliability
- **Power of Ten Compliant**: Follows NASA/JPL safety-critical coding standards
- **Compile-Time Verification**: `_Static_assert` validates all struct layouts and offsets
- **Bounded Operations**: All loops have fixed upper bounds (Rule 2)
- **No Dynamic Allocation**: After initialization, zero malloc/free calls (Rule 3)
- **Defensive Programming**: Minimum 2 assertions per function (Rule 5)
- **Return Value Checking**: All system calls verified (Rule 7)

## Performance Characteristics

| Metric | Socket Mode | DPDK Mode | Notes |
|--------|-------------|-----------|-------|
| UDP RX latency | ~5 µs | ~200 ns | Kernel bypass eliminates syscalls |
| Order latency | < 1 µs | < 1 µs | Engine processing unchanged |
| Timestamp overhead | ~5 cycles | ~5 cycles | Serialized `rdtscp` on x86-64 |
| Hash lookup | O(1) | O(1) | 1-2 cache lines |
| Memory per order | 64 bytes | 64 bytes | Exactly one cache line |
| Queue throughput | > 10M msgs/sec | > 10M msgs/sec | Lock-free SPSC |
| Packets per second | ~1M pps | ~10M+ pps | DPDK poll-mode driver |

## Cache Optimization Summary

Every data structure has been optimized for modern CPU cache hierarchies:

| Structure | Size | Alignment | Optimization |
|-----------|------|-----------|--------------|
| `order_t` | 64 bytes | 64-byte | One order = one cache line |
| `price_level_t` | 64 bytes | 64-byte | Hot fields in first 32 bytes |
| `order_map_slot_t` | 32 bytes | natural | 2 slots per cache line |
| `output_msg_envelope_t` | 64 bytes | 64-byte | Perfect for DMA transfers |
| `client_entry_t` | 64 bytes | 64-byte | Prevents false sharing in registry |
| `udp_client_entry_t` | 32 bytes | natural | 2 entries per cache line |
| `tcp_client_t` | optimized | 64-byte | Hot fields grouped first |
| Queue head/tail | 8 bytes each | 64-byte padding | Prevents false sharing |

## DPDK Kernel Bypass

### Overview

The matching engine supports optional DPDK (Data Plane Development Kit) integration for ultra-low-latency UDP packet I/O. DPDK bypasses the kernel network stack entirely, reducing UDP receive latency from ~5µs to ~200ns.

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Standard Socket Path (~5µs)                       │
│  NIC → IRQ → Kernel Driver → sk_buff → Socket Buffer → syscall → App│
└─────────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────────┐
│                    DPDK Kernel Bypass (~200ns)                       │
│  NIC → DMA to Huge Pages → Poll-Mode Driver → App                   │
└─────────────────────────────────────────────────────────────────────┘
```

### Architecture

The transport layer uses a clean abstraction that allows compile-time backend selection:

```
┌─────────────────────────────────────────────────────────────────────┐
│                    Application Code                                  │
│  udp_transport_t* t = udp_transport_create(&config, ...);           │
│  udp_transport_start(t);                                            │
│  udp_transport_send_to_client(t, client_id, data, len);             │
└──────────────────────────────┬──────────────────────────────────────┘
                               │
         ┌─────────────────────┴─────────────────────┐
         │                                           │
         ▼                                           ▼
┌─────────────────────┐                   ┌─────────────────────┐
│ Socket Backend      │                   │ DPDK Backend        │
│ (USE_DPDK=OFF)      │                   │ (USE_DPDK=ON)       │
├─────────────────────┤                   ├─────────────────────┤
│ • recvfrom()        │                   │ • rte_eth_rx_burst()│
│ • sendto()          │                   │ • rte_eth_tx_burst()│
│ • Works everywhere  │                   │ • Linux only        │
│ • No special setup  │                   │ • Requires sudo     │
│ • ~5µs latency      │                   │ • ~200ns latency    │
└─────────────────────┘                   └─────────────────────┘
```

### Hybrid Approach

The engine uses a hybrid network strategy:

| Protocol | Backend | Rationale |
|----------|---------|-----------|
| **UDP RX/TX** | DPDK | Hot path, connectionless, simple 8-byte header |
| **Multicast TX** | DPDK | Market data broadcast, high throughput |
| **TCP** | Kernel | Complex state machine (SYN/ACK/FIN), flow control |

### Platform Support

| Platform | DPDK Support | Notes |
|----------|--------------|-------|
| Linux (bare metal) | ✅ Full | Best performance |
| Linux (Cloud/AWS) | ✅ Full | ENA driver, dedicated instances |
| WSL2 | ❌ No | Virtualized network |
| macOS | ❌ No | Different kernel (XNU) |
| Windows | ❌ No | DPDK is Linux/FreeBSD only |

### DPDK Prerequisites (Linux)

```bash
# 1. Install DPDK
sudo apt update
sudo apt install -y dpdk dpdk-dev libdpdk-dev dpdk-doc

# 2. Allocate huge pages (2GB recommended)
sudo sh -c 'echo 1024 > /proc/sys/vm/nr_hugepages'

# Make permanent (survives reboot):
echo 'vm.nr_hugepages = 1024' | sudo tee -a /etc/sysctl.conf

# 3. Load UIO module
sudo modprobe uio_pci_generic

# Make permanent:
echo 'uio_pci_generic' | sudo tee -a /etc/modules-load.d/dpdk.conf

# 4. Verify setup
./build.sh check-dpdk
```

### Building with DPDK

```bash
# Standard build (sockets, works everywhere)
./build.sh build

# DPDK build with virtual device (no physical NIC needed)
./build.sh build-dpdk          # Uses net_null (TX benchmarks)
./build.sh build-dpdk ring     # Uses net_ring (loopback testing)

# DPDK build for physical NIC
./build.sh build-dpdk ""       # Empty string = physical NIC
```

### Running with DPDK

```bash
# Check prerequisites first
./build.sh check-dpdk

# Run with DPDK (requires sudo for huge pages)
./build.sh run-dpdk            # Interactive mode
./build.sh run-dpdk-quiet      # Benchmark mode
./build.sh run-dpdk-benchmark  # Binary + quiet

# Run DPDK tests
./build.sh test-dpdk

# DPDK benchmark
./build.sh benchmark-dpdk 24   # 10M matching pairs
```

### Virtual Device Testing

DPDK provides virtual devices for testing without a physical NIC:

| Device | Command | Use Case |
|--------|---------|----------|
| `net_null` | `./build.sh build-dpdk` | TX benchmarks, drops all packets |
| `net_ring` | `./build.sh build-dpdk ring` | Loopback testing, internal ring |

This allows full DPDK code path testing on any Linux system with huge pages.

### Binding a Physical NIC

When ready to use a real NIC:

```bash
# 1. Identify your NIC
dpdk-devbind.py --status

# 2. Bring interface down
sudo ip link set eth0 down

# 3. Bind to DPDK driver
sudo dpdk-devbind.py --bind=uio_pci_generic 0000:05:00.0

# 4. Build without virtual device
./build.sh build-dpdk ""

# 5. Run
./build.sh run-dpdk

# To unbind (return to kernel):
sudo dpdk-devbind.py --bind=igc 0000:05:00.0
sudo ip link set eth0 up
```

### Supported NICs

| Vendor | Driver | Models |
|--------|--------|--------|
| Intel | `igc` | I225-V, I226-V (2.5GbE) |
| Intel | `ixgbe` | X520, X540, X550 (10GbE) |
| Intel | `i40e` | X710, XL710, XXV710 (10/25/40GbE) |
| Intel | `ice` | E810 (25/100GbE) |
| Mellanox | `mlx5` | ConnectX-4/5/6 (10/25/40/100GbE) |
| Amazon | `ena` | Elastic Network Adapter (cloud) |

### DPDK File Structure

```
matching-engine-c/
├── include/network/
│   ├── transport_types.h        # Shared types (socket & DPDK)
│   ├── udp_transport.h          # Abstract UDP transport API
│   ├── multicast_transport.h    # Abstract multicast API
│   │
│   └── dpdk/
│       ├── dpdk_config.h        # Configuration constants
│       └── dpdk_init.h          # DPDK initialization API
│
├── src/network/
│   ├── udp_socket.c             # Socket backend
│   ├── multicast_socket.c       # Socket backend
│   │
│   └── dpdk/
│       ├── dpdk_init.c          # EAL, mempool, port setup
│       ├── dpdk_udp.c           # UDP via rte_eth_rx/tx_burst
│       └── dpdk_multicast.c     # Multicast via DPDK
│
├── tests/
│   └── test_dpdk.c              # DPDK transport tests
│
└── docs/
    └── KERNEL_BYPASS_SETUP.md   # Detailed setup guide
```

## Building

### Prerequisites
- GCC 7+ or Clang 6+ with C11 support
- CMake 3.10+
- Ninja (recommended) or Make
- Linux (for full performance) or macOS (with platform fallbacks)
- DPDK 21.11+ (optional, for kernel bypass)

### Quick Start

```bash
# Clone and build
git clone https://github.com/yourrepo/matching-engine-c.git
cd matching-engine-c

# Build with sockets (works everywhere)
./build.sh build

# Run tests
./build.sh test

# Start server
./build.sh run
```

### Build Commands

```bash
# Standard builds
./build.sh build           # Release with sockets
./build.sh debug           # Debug build
./build.sh rebuild         # Clean + rebuild

# DPDK builds (Linux only)
./build.sh build-dpdk      # Release with DPDK (net_null vdev)
./build.sh build-dpdk ring # Release with DPDK (net_ring vdev)

# Specialized builds
./build.sh valgrind        # Run tests under valgrind
```

### CMake Options

```bash
# Standard socket build
cmake .. -DCMAKE_BUILD_TYPE=Release

# DPDK kernel bypass
cmake .. -DUSE_DPDK=ON -DDPDK_VDEV=null

# Debug with AddressSanitizer
cmake .. -DCMAKE_BUILD_TYPE=Debug -DENABLE_ASAN=ON

# Valgrind-compatible (no AVX-512)
cmake .. -DVALGRIND_BUILD=ON
```

### Compiler Flags

The project compiles with strict warnings:
```
-Wall -Wextra -Wpedantic -Werror -march=native -O3
```

## Usage

### Server Modes

```bash
# Standard socket mode
./build.sh run              # CSV format, dual processor
./build.sh run-binary       # Binary format
./build.sh run-quiet        # Benchmark mode (minimal output)
./build.sh run-benchmark    # Binary + quiet

# DPDK kernel bypass mode (Linux, requires sudo)
./build.sh run-dpdk         # CSV format
./build.sh run-dpdk-benchmark # Binary + quiet
```

### Client Commands

```bash
./build.sh client           # Interactive TCP client
./build.sh client udp       # Interactive UDP client
./build.sh client-scenario 24 udp  # Run benchmark scenario
./build.sh scenarios        # List all scenarios
```

### Benchmark Commands

```bash
# Socket benchmarks
./build.sh benchmark-match 24    # 10M matching pairs
./build.sh benchmark-dual 26     # 250M pairs, dual processor

# DPDK benchmarks (requires sudo)
./build.sh benchmark-dpdk 24     # 10M pairs with kernel bypass
```

### Message Format

**Input Messages (CSV):**
```
# New Order: N, user_id, symbol, price, qty, side, order_id
N, 1, IBM, 150, 100, B, 1001

# Cancel: C, symbol, user_id, order_id  
C, IBM, 1, 1001

# Flush all books: F
F
```

**Input Messages (Binary):**
```
Magic (0x4D) | Type (1 byte) | Payload (variable)
New Order:  0x4D 0x01 [user_id:4][symbol:8][price:4][qty:4][side:1][order_id:4] = 27 bytes
Cancel:     0x4D 0x02 [user_id:4][order_id:4] = 10 bytes  
Flush:      0x4D 0x03 = 2 bytes
```

**Output Messages:**
```
# Acknowledgment
A, user_id, order_id

# Trade
T, buy_user, buy_order, sell_user, sell_order, price, qty, symbol

# Top of Book Update
B, symbol, side, price, qty

# Cancel Acknowledgment
X, user_id, order_id
```

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────────┐
│                         Network Layer                                │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────────┐  │
│  │ UDP Transport   │  │ TCP Listener    │  │ Multicast Transport │  │
│  │ ═══════════════ │  │ • TCP_NODELAY   │  │ ═══════════════════ │  │
│  │ Socket Backend: │  │ • TCP_QUICKACK  │  │ Socket Backend:     │  │
│  │ • SO_BUSY_POLL  │  │ • epoll/kqueue  │  │ • IP_MULTICAST_TTL  │  │
│  │ • 10MB rx buf   │  │ • edge-trigger  │  │ • Batch send        │  │
│  │ ─────────────── │  │                 │  │ ─────────────────── │  │
│  │ DPDK Backend:   │  │                 │  │ DPDK Backend:       │  │
│  │ • rx_burst()    │  │                 │  │ • tx_burst()        │  │
│  │ • Poll mode     │  │                 │  │ • Zero-copy         │  │
│  │ • ~200ns RX     │  │                 │  │ • Multicast MAC     │  │
│  └────────┬────────┘  └────────┬────────┘  └──────────▲──────────┘  │
└───────────┼─────────────────────┼─────────────────────┼─────────────┘
            │                     │                     │
            ▼                     ▼                     │
┌───────────────────────────────────────────────────────┼─────────────┐
│                  Lock-Free SPSC Queues                │             │
│  ┌─────────────────────────────┐  ┌───────────────────┼───────────┐ │
│  │ Input Queue                 │  │ Output Queue      │           │ │
│  │ • 64K × 64B envelopes       │  │ • 64K × 64B       │           │ │
│  │ • Batch dequeue (32/op)     │  │ • Non-atomic stats│           │ │
│  │ • Cache-line aligned        │  │ • Round-robin     ◄───────────┘ │
│  └──────────────┬──────────────┘  └───────────────────┘             │
└─────────────────┼───────────────────────────────────────────────────┘
                  │
                  ▼
┌─────────────────────────────────────────────────────────────────────┐
│                      Processor Thread                                │
│  • Batch dequeue up to 32 messages (single atomic op)               │
│  • Configurable spin-wait (PAUSE/YIELD) or nanosleep                │
│  • Prefetch next message while processing current                   │
└──────────────────────────────┬──────────────────────────────────────┘
                               │
                               ▼
┌─────────────────────────────────────────────────────────────────────┐
│                       Matching Engine                                │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │ Symbol Map (open-addressing, 512 slots)                       │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                              │                                       │
│              ┌───────────────┼───────────────┐                      │
│              ▼               ▼               ▼                      │
│  ┌───────────────┐ ┌───────────────┐ ┌───────────────┐             │
│  │ Order Book    │ │ Order Book    │ │ Order Book    │ ...         │
│  │ IBM           │ │ AAPL          │ │ NVDA          │             │
│  │ Bids/Asks     │ │ Bids/Asks     │ │ Bids/Asks     │             │
│  │ Order Map     │ │ Order Map     │ │ Order Map     │             │
│  └───────────────┘ └───────────────┘ └───────────────┘             │
│                                                                      │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │ Memory Pools (shared across all books)                        │  │
│  │ Order Pool: 10,000 × 64 bytes = 640 KB (pre-allocated)       │  │
│  │ Zero malloc/free in hot path (Rule 3 compliant)              │  │
│  └───────────────────────────────────────────────────────────────┘  │
└─────────────────────────────────────────────────────────────────────┘
```

## Dual-Processor Mode

For horizontal scaling, the engine supports two processor threads with symbol-based partitioning:

```
                    ┌──────────────────────┐
                    │ Symbol Router        │
                    │ Branchless:          │
                    │ return (int)is_n_to_z│
                    └───────────┬──────────┘
                                │
              ┌─────────────────┴─────────────────┐
              ▼                                   ▼
    ┌─────────────────────┐             ┌─────────────────────┐
    │ Processor 0         │             │ Processor 1         │
    │ Symbols: A-M        │             │ Symbols: N-Z        │
    │ Input Queue 0       │             │ Input Queue 1       │
    │ Output Queue 0      │             │ Output Queue 1      │
    └─────────────────────┘             └─────────────────────┘
```

## Project Structure

```
matching-engine-c/
├── include/
│   ├── core/
│   │   ├── order.h              # 64-byte cache-aligned order struct
│   │   ├── order_book.h         # Price levels, open-addressing order map
│   │   └── matching_engine.h    # Multi-symbol engine, symbol map
│   ├── protocol/
│   │   ├── message_types.h      # Packed uint8_t enums
│   │   ├── symbol_router.h      # Branchless A-M/N-Z routing
│   │   ├── binary/              # Binary protocol implementation
│   │   └── csv/                 # CSV protocol implementation
│   ├── threading/
│   │   ├── lockfree_queue.h     # SPSC queue with batch dequeue
│   │   ├── processor.h          # Batched stats, spin-wait config
│   │   └── queues.h             # Queue type instantiations
│   ├── network/
│   │   ├── transport_types.h    # Shared transport types
│   │   ├── udp_transport.h      # Abstract UDP API
│   │   ├── multicast_transport.h # Abstract multicast API
│   │   ├── tcp_listener.h       # TCP multi-client
│   │   └── dpdk/                # DPDK-specific headers
│   └── platform/
│       └── timestamps.h         # RDTSC / clock_gettime abstraction
├── src/
│   ├── core/                    # Matching logic
│   ├── protocol/                # Message parsing/formatting
│   ├── threading/               # Processor implementation
│   ├── network/
│   │   ├── udp_socket.c         # Socket backend
│   │   ├── multicast_socket.c   # Socket backend
│   │   └── dpdk/                # DPDK backend
│   └── modes/                   # Server mode entry points
├── tests/
│   ├── core/                    # Order book & engine tests
│   ├── protocol/                # Message parsing tests
│   ├── threading/               # Queue tests
│   └── test_dpdk.c              # DPDK transport tests
├── docs/
│   ├── ARCHITECTURE.md          # Design documentation
│   └── KERNEL_BYPASS_SETUP.md   # DPDK setup guide
├── CMakeLists.txt
└── build.sh                     # Build automation script
```

## Testing

```bash
# Run all tests
./build.sh test

# Run with valgrind
./build.sh valgrind

# Run DPDK tests (requires sudo)
./build.sh test-dpdk

# Memory checking
valgrind --leak-check=full ./build/matching_engine_tests
```

## Power of Ten Compliance

This codebase follows Gerard Holzmann's "Power of Ten" rules:

| Rule | Implementation | Verification |
|------|----------------|--------------|
| 1. No goto, setjmp, recursion | All control flow is structured | Code review |
| 2. Fixed loop bounds | `MAX_PROBE_LENGTH`, `MAX_MATCH_ITERATIONS` | `_Static_assert` |
| 3. No malloc after init | Memory pools pre-allocate everything | Code review |
| 4. Functions ≤ 60 lines | Large functions split | `wc -l` |
| 5. ≥ 2 assertions per function | Preconditions + postconditions | `grep -c assert` |
| 6. Smallest variable scope | Declared at point of use | C99 style |
| 7. Check all return values | pthread, clock_gettime, socket ops | Code review |
| 8. Limited preprocessor | Simple macros, no complex logic | `grep -c '#define'` |
| 9. Restrict pointer use | Temp variables eliminate `(*ptr)->field` | Code review |
| 10. Compile warning-free | `-Wall -Wextra -Wpedantic -Werror` | CI build |

### Assertion Counts by Module

| Module | Assertions | Notes |
|--------|------------|-------|
| core/ | ~120 | order.h, order_book.c, matching_engine.c |
| threading/ | ~80 | lockfree_queue.h, processor.c |
| protocol/ | ~100 | parsers, formatters, validators |
| network/ | ~230 | Socket + DPDK implementations |
| **Total** | **~530+** | Minimum 2 per function |

## Platform Support

| Feature | Linux x86-64 | macOS Intel | macOS ARM |
|---------|--------------|-------------|-----------|
| Socket mode | ✅ | ✅ | ✅ |
| DPDK mode | ✅ | ❌ | ❌ |
| Cache alignment | 64-byte | 64-byte | 64-byte |
| Timestamps | `rdtscp` | `rdtscp` | `clock_gettime` |
| Spin-wait hint | `PAUSE` | `PAUSE` | `YIELD` |
| Event loop | epoll | kqueue | kqueue |
| `TCP_QUICKACK` | ✅ | ❌ | ❌ |
| `SO_BUSY_POLL` | ✅ | ❌ | ❌ |

## References

- [Power of Ten - Rules for Developing Safety Critical Code](https://spinroot.com/gerard/pdf/P10.pdf) - Gerard Holzmann, NASA/JPL
- [What Every Programmer Should Know About Memory](https://www.akkadia.org/drepper/cpumemory.pdf) - Ulrich Drepper
- [Lock-Free Data Structures](https://www.cs.cmu.edu/~410-s05/lectures/L31_LockFree.pdf) - CMU
- [DPDK Programmer's Guide](https://doc.dpdk.org/guides/prog_guide/) - DPDK Project

## Changelog

### v2.2 (December 2024)
- **DPDK Kernel Bypass**: Full implementation with ~25x latency reduction
- **Transport Abstraction**: Clean API supporting socket and DPDK backends
- **Virtual Device Support**: net_null and net_ring for testing without physical NIC
- **DPDK Files**: dpdk_init.c, dpdk_udp.c, dpdk_multicast.c (2,347 lines, 74 assertions)
- **build.sh**: New commands: check-dpdk, build-dpdk, run-dpdk, test-dpdk
- **Documentation**: KERNEL_BYPASS_SETUP.md with complete setup guide

### v2.1 (December 2024)
- **Network**: Socket optimizations (`TCP_NODELAY`, `TCP_QUICKACK`, `SO_BUSY_POLL`)
- **Network**: Thread-local parsers/formatters (eliminates global state)
- **Network**: Kernel bypass abstraction points `[KB-1]` through `[KB-5]`
- **Protocol**: `_Static_assert` for all binary struct sizes and offsets
- **Protocol**: Truly branchless symbol routing (no ternary operator)

### v2.0 (December 2024)
- **Critical bug fix**: Iterator invalidation in `order_book_cancel_client_orders`
- **Critical bug fix**: RDTSC serialization (use `rdtscp` instead of `rdtsc`)
- **Performance**: Lock-free queue batch dequeue (~20-30x speedup)
- **Performance**: Non-atomic queue statistics (-45-90 cycles/message)
- **Safety**: Rule 5 compliance (minimum 2 assertions per function)
- **Compatibility**: macOS platform support (Intel and Apple Silicon)

### v1.0 (Initial Release)
- Core matching engine with price-time priority
- Lock-free SPSC queues
- UDP and TCP modes
- Dual-processor support
