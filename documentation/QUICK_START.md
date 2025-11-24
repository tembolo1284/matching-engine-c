# Quick Start Guide

Get up and running with the Matching Engine in 5 minutes.

## Table of Contents
- [Prerequisites](#prerequisites)
- [Building](#building)
- [Running the Server](#running-the-server)
- [Testing with Clients](#testing-with-clients)
- [Common Use Cases](#common-use-cases)
- [Interactive Mode](#interactive-mode)
- [Graceful Shutdown](#graceful-shutdown)

---

## Prerequisites

```bash
# Ubuntu/Debian
sudo apt-get install build-essential

# macOS
xcode-select --install

# Verify GCC with C11 support
gcc --version  # Should be 4.9+
```

---

## Building

```bash
# Quick build
make clean && make

# This creates:
#   build/matching_engine - Main server
#   build/tcp_client      - TCP test client
#   build/binary_client   - Binary protocol client
#   build/binary_decoder  - Binary to CSV decoder
```

For detailed build options, see [BUILD.md](BUILD.md).

---

## Running the Server

### TCP Mode (Recommended)

Start a TCP server that supports multiple simultaneous clients:

```bash
# Default port 1234, CSV output
./build/matching_engine --tcp

# Custom port with binary output
./build/matching_engine --tcp 5000 --binary

# With live binary decoder
./build/matching_engine --tcp --binary 2>/dev/null | ./build/binary_decoder
```

### UDP Mode (High-Throughput)

Start a UDP server for fire-and-forget messaging:

```bash
# Default port 1234
./build/matching_engine --udp

# Custom port
./build/matching_engine --udp 5000
```

### Command Line Options

```
Usage: ./build/matching_engine [--tcp|--udp] [port] [--binary]

Options:
  --tcp     : TCP mode with multi-client support (default)
  --udp     : UDP mode for high-throughput
  port      : Port to listen on (default: 1234)
  --binary  : Use binary protocol for output (default: CSV)

Examples:
  ./build/matching_engine --tcp              # TCP on port 1234, CSV output
  ./build/matching_engine --tcp 5000         # TCP on port 5000, CSV output
  ./build/matching_engine --tcp --binary     # TCP with binary output
  ./build/matching_engine --udp 1234         # UDP on port 1234
```

---

## Testing with Clients

### Quick Test - TCP with Pre-defined Scenario

**Terminal 1: Start Server**
```bash
./build/matching_engine --tcp 1234
```

**Terminal 2: Run Test Scenario**
```bash
# Scenario 2: Execute a matching trade
./build/tcp_client localhost 1234 2
```

**Expected Output:**
```
Connected to server
Assigned client_id: 1
Running Scenario 2: Trade execution

Sent: N, 1, IBM, 100, 50, B, 1
Server: A, IBM, 1, 1
Server: B, IBM, B, 100, 50

Sent: N, 1, IBM, 100, 50, S, 2
Server: A, IBM, 1, 2
Server: T, IBM, 1, 1, 1, 2, 100, 50
Server: B, IBM, B, -, -
Server: B, IBM, S, -, -

Sent: F
Server: (flush complete)
```

### Available Test Scenarios

```bash
./build/tcp_client localhost 1234 1  # Scenario 1: Simple orders
./build/tcp_client localhost 1234 2  # Scenario 2: Trade execution
./build/tcp_client localhost 1234 3  # Scenario 3: Order cancellation
```

---

## Common Use Cases

### Use Case 1: Single Client Testing

```bash
# Terminal 1: Server
./build/matching_engine --tcp

# Terminal 2: Interactive client
./build/tcp_client localhost 1234

# Type orders:
> N, 1, IBM, 100, 50, B, 1
> N, 1, IBM, 100, 50, S, 2
> F
> quit
```

### Use Case 2: Multi-Client Trading

```bash
# Terminal 1: Server
./build/matching_engine --tcp

# Terminal 2: Client 1 (assigned client_id=1)
./build/tcp_client localhost 1234
> N, 1, IBM, 100, 50, B, 1

# Terminal 3: Client 2 (assigned client_id=2)
./build/tcp_client localhost 1234
> N, 2, IBM, 100, 50, S, 1
```

**Both clients will receive the trade notification!**

### Use Case 3: Binary Protocol Testing

```bash
# Terminal 1: Server with binary output and decoder
./build/matching_engine --tcp --binary 2>/dev/null | ./build/binary_decoder

# Terminal 2: Binary client
./build/binary_client 1234 2 --tcp

# Or UDP mode
./build/matching_engine --udp --binary 2>/dev/null | ./build/binary_decoder
./build/binary_client 1234 2
```

### Use Case 4: High-Throughput UDP Testing

```bash
# Terminal 1: Server in UDP mode
./build/matching_engine --udp 1234

# Terminal 2: Fire multiple scenarios rapidly
./build/binary_client 1234 1
./build/binary_client 1234 2
./build/binary_client 1234 3
```

---

## Interactive Mode

When using TCP mode without a scenario number, you enter interactive mode:

### Available Commands

```
N, <userId>, <symbol>, <price>, <qty>, <side>, <userOrderId>  # New order
C, <userId>, <userOrderId>                                     # Cancel
F                                                              # Flush
help                                                           # Show commands
quit                                                           # Exit
```

Or use simplified syntax:
```
buy <symbol> <price> <qty> <order_id>     # Place buy order
sell <symbol> <price> <qty> <order_id>    # Place sell order
cancel <order_id>                          # Cancel order
flush                                      # Clear all books
```

### Example Session

```bash
$ ./build/tcp_client localhost 1234

Connected to server
Assigned client_id: 1
Enter orders (or 'quit' to exit):

> buy IBM 100 50 1
Sent: BUY IBM 50 @ 100 (order 1)
A, IBM, 1, 1
B, IBM, B, 100, 50

> sell IBM 100 50 2
Sent: SELL IBM 50 @ 100 (order 2)
A, IBM, 1, 2
T, IBM, 1, 1, 1, 2, 100, 50
B, IBM, B, -, -
B, IBM, S, -, -

> flush
Sent: FLUSH

> quit
Disconnected.
```

---

## Graceful Shutdown

Press `Ctrl+C` to initiate graceful shutdown:

```
^C
Received signal 2, shutting down gracefully...

=== Final Statistics ===
TCP Listener:
  Clients connected: 3
  Messages received: 150
  Messages sent: 380

Processor:
  Messages processed: 150
  Batches processed: 8

=== Matching Engine Stopped ===
```

---

## Quick Reference

| Server Command | Client Command | Protocol | Output | Mode |
|----------------|----------------|----------|--------|------|
| `--tcp 1234` | `localhost 1234` | CSV | CSV | Interactive |
| `--tcp --binary` | `localhost 1234` | CSV | Binary | Interactive |
| `--tcp 1234` | `localhost 1234 2` | CSV | CSV | Scenario + Interactive |
| `--udp 1234` | `1234 2` | Binary | CSV | Fire & forget |
| `--udp --binary` | `1234 2` | Binary | Binary | Fire & forget |

---

## What's Next?

- Learn about the system architecture: [ARCHITECTURE.md](ARCHITECTURE.md)
- Understand the protocols: [PROTOCOLS.md](PROTOCOLS.md)
- Run comprehensive tests: [TESTING.md](TESTING.md)
- Explore build options: [BUILD.md](BUILD.md)

---

## Troubleshooting

### Port Already in Use
```bash
# Check what's using port 1234
lsof -i :1234

# Use a different port
./build/matching_engine --tcp 5000
```

### Connection Refused
```bash
# Ensure server is running first
./build/matching_engine --tcp

# Then connect from another terminal
./build/tcp_client localhost 1234
```

### Build Errors
```bash
# Clean and rebuild
make clean
make

# See BUILD.md for detailed troubleshooting
```
