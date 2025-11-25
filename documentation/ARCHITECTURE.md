# Architecture

Comprehensive system design of the Matching Engine, emphasizing **zero-allocation memory pools**, lock-free threading, **dual-processor symbol partitioning**, **multicast market data broadcasting**, **envelope-based message routing**, and production-grade architecture.

## Table of Contents
- [System Overview](#system-overview)
- [Dual-Processor Architecture](#dual-processor-architecture)
- [Multicast Market Data Feed](#multicast-market-data-feed)
- [Memory Pool System](#memory-pool-system)
- [Threading Model](#threading-model)
- [Symbol Router](#symbol-router)
- [Envelope Pattern](#envelope-pattern)
- [Data Flow](#data-flow)
- [Core Components](#core-components)
- [TCP Multi-Client Architecture](#tcp-multi-client-architecture)
- [Message Framing (TCP)](#message-framing-tcp)
- [Modular Code Structure](#modular-code-structure)
- [Design Decisions](#design-decisions)
- [Performance Characteristics](#performance-characteristics)
- [Project Structure](#project-structure)

---

## System Overview

The Matching Engine is a **production-grade** order matching system built in pure C11. The defining characteristics are:

1. **Dual-processor symbol partitioning** - Horizontal scaling via A-M / N-Z routing
2. **Zero-allocation hot path** - All memory pre-allocated in pools
3. **Multicast market data broadcasting** - Industry-standard UDP multicast for market data distribution
4. **Envelope-based routing** - Messages wrapped with client metadata for multi-client support
5. **Lock-free communication** - SPSC queues between threads
6. **Client isolation** - TCP clients validated and isolated
7. **Modular architecture** - Separate files for TCP, UDP, and multicast modes

### High-Level Architecture (Dual-Processor + Multicast)
```
┌────────────────────────────────────────────────────────────────────────────────────────┐
│                    Matching Engine - Dual Processor + Multicast Mode                     │
│                          Zero-Allocation Memory Pools                                    │
├────────────────────────────────────────────────────────────────────────────────────────┤
│                                                                                          │
│  TCP MODE (4-5 Threads):                                                                 │
│                                                                                          │
│  ┌──────────────┐                                                                        │
│  │ TCP Listener │                                                                        │
│  │  (Thread 1)  │                                                                        │
│  │              │                                                                        │
│  │ epoll/kqueue │         Symbol-Based Routing                                           │
│  │ event loop   │        ┌─────────────────────┐                                         │
│  │              │        │  First char A-M?    │                                         │
│  └──────┬───────┘        │  → Processor 0      │                                         │
│         │                │  First char N-Z?    │                                         │
│         │                │  → Processor 1      │                                         │
│         │                └─────────────────────┘                                         │
│         │                                                                                │
│         ├──────────────────────┬──────────────────────┐                                 │
│         │                      │                      │                                 │
│         ▼                      ▼                      │                                 │
│  ┌─────────────────┐    ┌─────────────────┐          │                                 │
│  │ Input Queue 0   │    │ Input Queue 1   │          │                                 │
│  │  (A-M symbols)  │    │  (N-Z symbols)  │          │                                 │
│  └────────┬────────┘    └────────┬────────┘          │                                 │
│           │                      │                    │                                 │
│           ▼                      ▼                    │                                 │
│  ┌─────────────────┐    ┌─────────────────┐          │                                 │
│  │  Processor 0    │    │  Processor 1    │          │                                 │
│  │   (Thread 2)    │    │   (Thread 3)    │          │                                 │
│  │                 │    │                 │          │                                 │
│  │ Memory Pool 0   │    │ Memory Pool 1   │          │                                 │
│  │ Engine 0 (A-M)  │    │ Engine 1 (N-Z)  │          │                                 │
│  └────────┬────────┘    └────────┬────────┘          │                                 │
│           │                      │                    │                                 │
│           ▼                      ▼                    │                                 │
│  ┌─────────────────┐    ┌─────────────────┐          │                                 │
│  │ Output Queue 0  │    │ Output Queue 1  │          │                                 │
│  └────────┬────────┘    └────────┬────────┘          │                                 │
│           │                      │                    │                                 │
│           └──────────┬───────────┘                    │                                 │
│                      │                                │                                 │
│           ┌──────────┼────────────┐                   │                                 │
│           │          │            │                   │                                 │
│           ▼          ▼            ▼                   │                                 │
│   ┌───────────────┐  │  ┌──────────────────┐         │                                 │
│   │ Output Router │  │  │ Multicast Pub.   │         │                                 │
│   │  (Thread 4)   │  │  │   (Thread 5)     │ ◄───────┘ OPTIONAL                        │
│   │               │  │  │    OPTIONAL      │                                            │
│   │ Round-robin   │  │  │                  │                                            │
│   │ from queues   │  │  │ UDP Multicast    │                                            │
│   └───────┬───────┘  │  │ 239.255.0.1      │                                            │
│           │          │  └────────┬─────────┘                                            │
│           │          │           │                                                      │
│  ┌────────┴──────────┴───────┐   │                                                      │
│  │   To TCP Clients          │   │                                                      │
│  │   (per-client queues)     │   │                                                      │
│  └───────────────────────────┘   │                                                      │
│                                  │                                                      │
│                                  ▼                                                      │
│                    ┌─────────────────────────────┐                                      │
│                    │  Multicast Subscribers      │                                      │
│                    │  (Unlimited, zero overhead) │                                      │
│                    │                             │                                      │
│                    │  ▸ Subscriber 1 (Machine A) │                                      │
│                    │  ▸ Subscriber 2 (Machine A) │                                      │
│                    │  ▸ Subscriber 3 (Machine B) │                                      │
│                    │  ▸ Subscriber N ...         │                                      │
│                    └─────────────────────────────┘                                      │
│                                                                                          │
│  UDP MODE (3-4 Threads): Similar structure without TCP listener                          │
│                                                                                          │
└────────────────────────────────────────────────────────────────────────────────────────┘
```

**Key Design Principles:**
- **Horizontal scaling** - Dual processors double throughput potential
- **Zero malloc/free in hot path** - All memory pre-allocated in pools
- **Symbol-based partitioning** - Orders route by symbol's first character
- **Lock-free communication** - SPSC queues between threads
- **Separate memory pools** - Each processor has isolated memory
- **Round-robin output** - Fair scheduling from multiple output queues
- **Multicast broadcasting** - Optional market data feed for unlimited subscribers

---

## Multicast Market Data Feed

### Overview

The multicast publisher is an **optional 5th thread** that broadcasts market data (trades, top-of-book updates, acknowledgments) to a UDP multicast group. This is the **industry-standard pattern** used by real exchanges like CME, NASDAQ, and ICE for distributing market data to unlimited subscribers with zero server overhead.

### Why Multicast?

**Traditional Unicast (N TCP connections):**
```
Server → Client 1  (send once)
Server → Client 2  (send once)
Server → Client 3  (send once)
...
Server → Client N  (send once)
```
**Cost:** N sends, N TCP state machines, N × bandwidth

**Multicast (UDP multicast group):**
```
Server → Multicast Group 239.255.0.1:5000  (send ONCE)
         └─→ Network delivers to ALL subscribers
```
**Cost:** 1 send, zero per-subscriber overhead, 1 × bandwidth

### Multicast Architecture
```
┌────────────────────────────────────────────────────────────────┐
│           Multicast Publisher (Optional 5th Thread)             │
├────────────────────────────────────────────────────────────────┤
│                                                                 │
│  Input: Reads from processor output queues (round-robin)       │
│                                                                 │
│  ┌─────────────────┐    ┌─────────────────┐                   │
│  │ Output Queue 0  │    │ Output Queue 1  │                   │
│  │  (Processor 0)  │    │  (Processor 1)  │                   │
│  └────────┬────────┘    └────────┬────────┘                   │
│           │                      │                             │
│           └──────────┬───────────┘                             │
│                      │                                         │
│                      ▼                                         │
│            ┌──────────────────┐                                │
│            │ Multicast Pub.   │                                │
│            │  (Thread 5)      │                                │
│            │                  │                                │
│            │  Round-robin     │                                │
│            │  Batch size: 32  │                                │
│            │  Format: CSV or  │                                │
│            │          Binary  │                                │
│            └────────┬─────────┘                                │
│                     │                                          │
│                     ▼                                          │
│           UDP Socket (sendto)                                  │
│           Multicast Group: 239.255.0.1:5000                    │
│           TTL: 1 (local subnet)                                │
│                     │                                          │
│                     └──────────────────┐                       │
│                                        │                       │
│                     Network Layer      │                       │
│                     (Switch/Router)    │                       │
│                                        │                       │
│            ┌────────────┬──────────────┴──────────────┐        │
│            │            │                             │        │
│            ▼            ▼                             ▼        │
│      Subscriber 1   Subscriber 2    ...      Subscriber N      │
│      (Machine A)    (Machine A)              (Machine B)       │
│                                                                 │
└────────────────────────────────────────────────────────────────┘
```

### Multicast Publisher Features

1. **Round-Robin Fairness**
   - Reads from both processor output queues
   - 32-message batch size per queue (same as output router)
   - Prevents one processor from monopolizing broadcast

2. **Protocol Support**
   - **CSV Mode:** Human-readable market data
   - **Binary Mode:** Compact binary protocol (lower bandwidth)

3. **Multicast Group Configuration**
   - Default: `239.255.0.1:5000` (organization-local scope)
   - TTL: 1 (local subnet only for safety)
   - Can be configured via `--multicast <group>:<port>`

4. **Statistics Tracking**
   - Packets sent
   - Messages broadcast
   - Per-processor message counts
   - Send errors

### Multicast Subscriber

The **multicast_subscriber** tool joins the multicast group and receives market data:
```bash
# Start subscribers (can run multiple instances)
./build/multicast_subscriber 239.255.0.1 5000
./build/multicast_subscriber 239.255.0.1 5000  # 2nd subscriber
./build/multicast_subscriber 239.255.0.1 5000  # 3rd subscriber
```

**Features:**
- Auto-detects CSV vs Binary protocol
- Real-time market data display
- Statistics tracking (packets, messages, parse errors)
- Throughput calculation (messages/sec)
- Signal handling (Ctrl+C shows statistics)

### Multicast Address Ranges

| Range | Scope | Use Case |
|-------|-------|----------|
| 224.0.0.0 - 224.0.0.255 | Link-local | Reserved (e.g., routing protocols) |
| 239.0.0.0 - 239.255.255.255 | Organization-local | **Perfect for LANs** (our default) |
| 224.0.1.0 - 238.255.255.255 | Internet-wide | Global multicast (requires infrastructure) |

### Usage Example
```bash
# Terminal 1: Start server with multicast
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Terminal 2: Start subscriber (can start multiple)
./build/multicast_subscriber 239.255.0.1 5000

# Terminal 3: Send orders
./build/tcp_client localhost 1234
> buy IBM 100 50 1
> sell IBM 100 30 2
```

**Result:** ALL subscribers receive trades, top-of-book updates, and acknowledgments simultaneously!

### Real-World Analogy

**Multicast is like a TV broadcast:**
- **239.255.0.1** = TV Channel (multicast group address)
- **Server** = TV Station (broadcasts once)
- **Subscribers** = TVs (unlimited viewers, zero station overhead)
- **Network** = Cable/Satellite Network (handles distribution)

---

## Modular Code Structure

The codebase is organized into **modular components** for maintainability and clarity:

### Main Dispatcher (`src/main.c`)

**~150 lines** - Slim entry point that parses arguments and dispatches to appropriate mode:
```c
int main(int argc, char* argv[]) {
    // Parse command-line arguments
    app_config_t config = parse_args(argc, argv);
    
    // Setup signal handlers
    setup_signal_handlers();
    
    // Dispatch to appropriate mode
    if (config.tcp_mode) {
        if (config.dual_processor) {
            return run_tcp_dual_processor(&config);
        } else {
            return run_tcp_single_processor(&config);
        }
    } else {
        if (config.dual_processor) {
            return run_udp_dual_processor(&config);
        } else {
            return run_udp_single_processor(&config);
        }
    }
}
```

### Mode Implementations

Each mode has its own file with complete implementation:

#### TCP Mode (`src/modes/tcp_mode.c`)
- `run_tcp_dual_processor()` - 4-5 threads (with optional multicast)
- `run_tcp_single_processor()` - 3-4 threads (with optional multicast)
- Thread lifecycle management
- Resource allocation and cleanup
- Statistics printing

#### UDP Mode (`src/modes/udp_mode.c`)
- `run_udp_dual_processor()` - 4 threads with round-robin output from BOTH processors
- `run_udp_single_processor()` - 3 threads
- Similar structure to TCP mode

#### Multicast Helpers (`src/modes/multicast_helpers.c`)
- `multicast_setup_single()` - Setup for single processor
- `multicast_setup_dual()` - Setup for dual processor
- `multicast_start()` - Start publisher thread
- `multicast_cleanup()` - Stop and cleanup
- Encapsulates all multicast configuration

#### Shared Helpers (`src/modes/helpers.c`)
- `print_memory_stats()` - Display pool statistics
- Common utilities shared across modes

### Benefits of Modular Structure

| Benefit | Description |
|---------|-------------|
| **Maintainability** | Each mode is self-contained and easy to understand |
| **Testability** | Can test modes independently |
| **Clarity** | Clear separation of concerns |
| **Extensibility** | Easy to add new modes (e.g., IPC mode) |
| **Compile Time** | Only rebuild affected modules |

---

## Project Structure
```
matching-engine-c/
├── include/
│   ├── core/
│   │   ├── order.h
│   │   ├── order_book.h
│   │   └── matching_engine.h
│   ├── protocol/
│   │   ├── message_types.h
│   │   ├── symbol_router.h                # Symbol routing logic
│   │   ├── csv/
│   │   │   ├── message_parser.h
│   │   │   └── message_formatter.h
│   │   └── binary/
│   │       ├── binary_message_parser.h
│   │       └── binary_message_formatter.h
│   ├── network/
│   │   ├── tcp_listener.h                 # Dual-processor support
│   │   ├── udp_receiver.h                 # Dual-processor support
│   │   ├── multicast_publisher.h          # NEW: Multicast broadcasting
│   │   └── ...
│   ├── threading/
│   │   ├── processor.h
│   │   ├── output_router.h                # Multi-queue support
│   │   └── ...
│   └── modes/
│       ├── run_modes.h                    # NEW: Config + helpers
│       ├── tcp_mode.h                     # NEW: TCP mode declarations
│       ├── udp_mode.h                     # NEW: UDP mode declarations
│       └── multicast_helpers.h            # NEW: Multicast setup
├── src/
│   ├── main.c                             # Slim dispatcher (~150 lines)
│   ├── core/
│   │   ├── order_book.c
│   │   └── matching_engine.c
│   ├── protocol/
│   │   ├── csv/
│   │   └── binary/
│   ├── network/
│   │   ├── tcp_listener.c
│   │   ├── udp_receiver.c
│   │   └── multicast_publisher.c          # NEW: Multicast implementation
│   ├── threading/
│   │   ├── processor.c
│   │   ├── output_router.c
│   │   └── ...
│   └── modes/                             # NEW: Modular mode implementations
│       ├── tcp_mode.c                     # TCP single/dual processor
│       ├── udp_mode.c                     # UDP single/dual processor
│       ├── multicast_helpers.c            # Multicast setup/teardown
│       └── helpers.c                      # Shared utilities
├── tools/
│   ├── tcp_client.c
│   ├── binary_client.c
│   ├── binary_decoder.c
│   └── multicast_subscriber.c             # NEW: Multicast receiver tool
├── tests/
│   └── ...
├── documentation/
│   ├── ARCHITECTURE.md                    # This file
│   ├── PROTOCOL.md
│   ├── BUILD.md
│   └── README.md
├── CMakeLists.txt                         # CMake build configuration
├── build.sh                               # Build script with all targets
└── ...
```

---

## Command-Line Options
```bash
# Dual-processor mode (DEFAULT)
./build/matching_engine --tcp
./build/matching_engine --tcp --dual-processor

# Single-processor mode
./build/matching_engine --tcp --single-processor

# Multicast mode (TCP + multicast broadcasting)
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Binary multicast
./build/matching_engine --tcp --binary --multicast 239.255.0.1:5000

# UDP modes
./build/matching_engine --udp
./build/matching_engine --udp --dual-processor

# Multicast subscriber (unlimited instances)
./build/multicast_subscriber 239.255.0.1 5000
```

---

## Performance Characteristics

### Throughput

| Mode | Throughput | Notes |
|------|-----------|-------|
| Single-Processor | 1-5M orders/sec | CPU limited |
| Dual-Processor | 2-10M orders/sec | Near-linear scaling |
| Memory Pool Alloc | 100M ops/sec | Just index manipulation |
| Multicast Broadcast | Unlimited subscribers | Zero per-subscriber overhead |

### Multicast Scaling

| Subscribers | Server Bandwidth | Server CPU | Network Bandwidth |
|-------------|------------------|------------|-------------------|
| 1 | 1× | Constant | 1× |
| 10 | 1× | Constant | 1× |
| 100 | 1× | Constant | 1× |
| 1000 | 1× | Constant | 1× |

**Key Point:** Multicast bandwidth and CPU are **independent of subscriber count**!

---

## See Also

- [Quick Start Guide](QUICK_START.md) - Get up and running
- [Protocols](PROTOCOL.MD) - Message format specifications
- [Testing](TESTING.md) - Comprehensive testing guide including multicast tests
- [Build Instructions](BUILD.md) - Detailed build guide with CMake
