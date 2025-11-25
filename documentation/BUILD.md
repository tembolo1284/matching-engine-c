# Build Instructions

Detailed guide for building the Matching Engine on various platforms.

## Table of Contents
- [Prerequisites](#prerequisites)
- [Quick Build](#quick-build)
- [Build Systems](#build-systems)
- [Build Targets](#build-targets)
- [Build Options](#build-options)
- [Processor Modes](#processor-modes)
- [Multicast Support](#multicast-support)
- [Platform-Specific Notes](#platform-specific-notes)
- [Troubleshooting](#troubleshooting)
- [Development Setup](#development-setup)

---

## Prerequisites

### Required Tools

| Tool | Minimum Version | Purpose |
|------|----------------|---------|
| GCC | 4.9+ | C11 support (`<stdatomic.h>`) |
| CMake | 3.15+ | Build system |
| Ninja | (optional) | Fast CMake backend |
| POSIX System | - | pthreads, sockets |

### Optional Tools

| Tool | Purpose |
|------|---------|
| Valgrind | Memory leak detection (Linux) |
| GDB | Debugging |
| clang-format | Code formatting |

---

## Quick Build

### Using build.sh (Recommended)
```bash
# Clean and build everything
./build.sh build

# Build artifacts in build/ directory:
#   - matching_engine          (main server)
#   - tcp_client              (TCP test client)
#   - binary_client           (binary protocol client)
#   - binary_decoder          (binary→CSV decoder)
#   - multicast_subscriber    (multicast receiver tool)
#   - matching_engine_tests   (unit tests)
```

### Using CMake Directly
```bash
# Generate build files
cmake -B build -G Ninja

# Build
cmake --build build

# Build artifacts in build/ directory (same as build.sh)
```

---

## Build Systems

### build.sh Script (Recommended)

The `build.sh` script provides a unified interface for building and testing:
```bash
# Build commands
./build.sh build            # Release build
./build.sh debug            # Debug build
./build.sh release          # Explicit release build
./build.sh rebuild          # Clean + rebuild
./build.sh clean            # Remove build directory
./build.sh clean-all        # Remove all build directories

# Test commands
./build.sh test             # Run unit tests
./build.sh test-dual-processor    # Test dual-processor routing
./build.sh test-single-processor  # Test single-processor mode
./build.sh test-multicast   # Test multicast market data feed
./build.sh valgrind         # Memory leak detection

# Run commands
./build.sh run              # Start server (TCP, dual-processor)
./build.sh run-tcp          # TCP mode (dual-processor)
./build.sh run-dual         # Explicit dual-processor mode
./build.sh run-single       # Single-processor mode
./build.sh run-udp          # UDP mode
./build.sh run-multicast    # TCP + multicast feed
./build.sh run-multicast-binary  # TCP + binary multicast

# Info
./build.sh info             # Show build configuration
./build.sh help             # Show all commands
```

**Advantages:**
- Simple interface
- Handles CMake configuration
- Includes test modes
- Platform detection (Linux/macOS)

### CMake (Direct)
```bash
# Configure (first time)
cmake -B build -G Ninja

# Reconfigure (after CMakeLists.txt changes)
cmake -B build

# Build specific targets
cmake --build build --target matching_engine
cmake --build build --target test-unit

# Clean
cmake --build build --target clean
```

**Advantages:**
- Platform-independent
- IDE integration (CLion, VS Code)
- Dependency management
- Modern build tool

---

## Build Targets

### Main Executables
```bash
# Build main server only
cmake --build build --target matching_engine

# Build test client
cmake --build build --target tcp_client

# Build binary tools
cmake --build build --target binary_client
cmake --build build --target binary_decoder

# Build multicast subscriber
cmake --build build --target multicast_subscriber

# Build tests
cmake --build build --target matching_engine_tests

# Build everything (default)
cmake --build build
```

### Test Targets
```bash
# Run unit tests
cmake --build build --target test-unit

# Run integration tests
cmake --build build --target test-tcp
cmake --build build --target test-binary

# Run dual-processor tests
cmake --build build --target test-dual-processor
cmake --build build --target test-single-processor

# Run multicast test
cmake --build build --target test-multicast

# Run all tests
cmake --build build --target test-all

# Memory analysis
cmake --build build --target valgrind
```

### Run Targets
```bash
# Run in TCP mode (dual-processor, default)
cmake --build build --target run
cmake --build build --target run-tcp

# Run in specific processor modes
cmake --build build --target run-dual
cmake --build build --target run-single

# Run in UDP mode
cmake --build build --target run-udp

# Run with binary output
cmake --build build --target run-binary
cmake --build build --target run-binary-decoded

# Run with multicast
cmake --build build --target run-multicast
cmake --build build --target run-multicast-binary
```

---

## Build Options

### Compiler Flags

#### Release Build (Default)
```cmake
CMAKE_C_FLAGS = "-Wall -Wextra -Wpedantic -Werror -pthread -O3"
```

- `-Wall -Wextra -Wpedantic` - Enable all warnings
- `-Werror` - Treat warnings as errors
- `-pthread` - POSIX threads support
- `-O3` - Maximum optimization

#### Debug Build
```bash
./build.sh debug
# Uses: -g -O0 -DDEBUG
```

- `-g` - Debug symbols for GDB
- `-O0` - No optimization (easier debugging)
- `-DDEBUG` - Enable debug assertions

#### Platform-Specific Flags

**Linux:**
```cmake
-march=native    # Optimize for current CPU
```

**macOS:**
```cmake
-Wno-deprecated-declarations    # Suppress macOS API warnings
```

#### Valgrind-Compatible Build
```bash
# Build without AVX-512 for Valgrind compatibility
./build.sh valgrind-build

# Or directly with CMake
cmake -B build-valgrind -DVALGRIND_BUILD=ON
cmake --build build-valgrind
```

### Build Types
```bash
# Release (default) - Maximum optimization
./build.sh build

# Debug - Debug symbols, no optimization
./build.sh debug

# Release from scratch
./build.sh rebuild
```

---

## Processor Modes

The matching engine supports two processor modes that affect runtime behavior but not compilation:

### Dual-Processor Mode (Default)
```bash
# Start in dual-processor mode
./build/matching_engine --tcp
./build/matching_engine --tcp --dual-processor

# Test dual-processor routing
./build.sh test-dual-processor
cmake --build build --target test-dual-processor
```

**Features:**
- Orders partitioned by symbol (A-M → P0, N-Z → P1)
- 4 threads: Receiver, Processor 0, Processor 1, Output Router
- 2x throughput potential (2-10M orders/sec)
- Separate memory pools per processor

### Single-Processor Mode
```bash
# Start in single-processor mode
./build/matching_engine --tcp --single-processor

# Test single-processor mode
./build.sh test-single-processor
cmake --build build --target test-single-processor
```

**Features:**
- All symbols to one processor
- 3 threads: Receiver, Processor, Output Router
- Baseline throughput (1-5M orders/sec)
- Backward compatible with original design

### Build Differences

**Important:** Both modes use the same compiled binary. The mode is selected at runtime via command-line flags:
```bash
# Same binary, different modes
./build/matching_engine --tcp --dual-processor
./build/matching_engine --tcp --single-processor
```

No special build flags or separate compilation needed.

---

## Multicast Support

### Overview

Multicast market data broadcasting is built into the main executable and enabled via command-line flags.

### Building Multicast Components
```bash
# Build everything (includes multicast support)
./build.sh build

# Build multicast subscriber tool specifically
cmake --build build --target multicast_subscriber
```

### Running with Multicast
```bash
# Server with multicast (CSV output)
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Server with binary multicast
./build/matching_engine --tcp --binary --multicast 239.255.0.1:5000

# Multicast subscriber (can run multiple instances)
./build/multicast_subscriber 239.255.0.1 5000
```

### Multicast Test
```bash
# Interactive test (provides instructions)
./build.sh test-multicast

# Or via CMake
cmake --build build --target test-multicast
```

### Multicast Configuration

| Parameter | Default | Description |
|-----------|---------|-------------|
| Group | 239.255.0.1 | Multicast group address |
| Port | 5000 | Multicast port |
| TTL | 1 | Time-to-live (local subnet) |
| Protocol | CSV | Can be Binary with `--binary` flag |

### Thread Count with Multicast

| Mode | Without Multicast | With Multicast |
|------|-------------------|----------------|
| Single-Processor | 3 threads | 4 threads |
| Dual-Processor | 4 threads | 5 threads |

---

## Platform-Specific Notes

### Linux (Ubuntu/Debian)

#### Install Prerequisites
```bash
# Build essentials
sudo apt-get update
sudo apt-get install build-essential cmake ninja-build

# Optional: Valgrind
sudo apt-get install valgrind

# Verify installation
gcc --version    # Should show 4.9+
cmake --version  # Should show 3.15+
```

#### Build
```bash
# Standard build
./build.sh build

# Everything works out of the box
```

#### Multicast on Linux
Multicast works out of the box on most Linux distributions. If you have issues:
```bash
# Check if multicast is enabled on interface
ip maddr show

# Add route for multicast (if needed)
sudo ip route add 239.0.0.0/8 dev eth0
```

#### Known Issues
- **None** - Linux is the primary development platform

### macOS

#### Install Prerequisites
```bash
# Install Xcode Command Line Tools
xcode-select --install

# Or install via Homebrew
brew install cmake ninja

# Verify installation
gcc --version
cmake --version
```

#### Build
```bash
# Standard build
./build.sh build

# Note: macOS uses Clang by default (works fine)
```

#### Multicast on macOS
Multicast typically works on macOS. For local testing:
```bash
# Multicast works on loopback by default
./build/matching_engine --tcp --multicast 239.255.0.1:5000
./build/multicast_subscriber 239.255.0.1 5000
```

#### Known Issues

**Issue 1: Valgrind Not Available**
- Valgrind doesn't work on modern macOS (Apple Silicon)
- Use `leaks` tool instead:
```bash
# macOS leaks tool
leaks --atExit -- ./build/matching_engine --tcp
```

The `build.sh valgrind` target automatically uses `leaks` on macOS.

**Issue 2: Network buffer sizes**
- macOS has smaller default network buffers
- May see dropped UDP packets under high load
- Workaround: Use TCP mode or increase buffer sizes

### Other POSIX Systems (FreeBSD, etc.)

#### Prerequisites
```bash
# Install via package manager
pkg install gcc cmake ninja  # FreeBSD
```

#### Build
```bash
./build.sh build
```

#### Known Issues
- May need to adjust `#include <sys/socket.h>` headers
- Check `man pthread` for POSIX compliance
- Network constants may differ (`SOL_SOCKET`, etc.)

### Windows (WSL)

#### Windows Subsystem for Linux (Recommended)
```bash
# In WSL Ubuntu
sudo apt-get install build-essential cmake ninja-build
./build.sh build
# Works exactly like Linux
```

#### Native Windows
- Not currently supported (requires Winsock2 adaptations)
- **Recommendation:** Use WSL instead

---

## Troubleshooting

### Build Errors

#### Error: "undefined reference to pthread_create"
```
/usr/bin/ld: ... undefined reference to `pthread_create'
```

**Solution:** Missing `-pthread` flag (should be automatic in CMake)
```bash
# Force rebuild
./build.sh rebuild
```

#### Error: "stdatomic.h: No such file or directory"
```
src/lockfree_queue.h:5:10: fatal error: stdatomic.h: No such file or directory
```

**Solution:** Compiler doesn't support C11
```bash
# Check GCC version
gcc --version  # Need 4.9+

# Update GCC
sudo apt-get install gcc-11  # Ubuntu
brew install gcc             # macOS

# Specify compiler
CC=gcc-11 ./build.sh build
```

#### Error: "CMake 3.15 or higher is required"
```
CMake Error: CMake 3.15 or higher is required.
```

**Solution:** Update CMake
```bash
# Ubuntu
sudo apt-get install cmake

# Or download from cmake.org

# macOS
brew install cmake
```

#### Error: "Ninja not found"
```
CMake Error: Could not find CMAKE_MAKE_PROGRAM
```

**Solution:** Install Ninja or use Unix Makefiles
```bash
# Install Ninja
sudo apt-get install ninja-build  # Ubuntu
brew install ninja                # macOS

# Or use Make instead
cmake -B build -G "Unix Makefiles"
cmake --build build
```

### Runtime Errors

#### Error: "Address already in use"
```
Error binding socket: Address already in use
```

**Solution:** Port already bound
```bash
# Find process using port
lsof -i :1234

# Kill it
kill <PID>

# Or use different port
./build/matching_engine --tcp 5000
```

#### Multicast: "Cannot assign requested address"
```
setsockopt IP_ADD_MEMBERSHIP failed: Cannot assign requested address
```

**Solution:** Network interface doesn't support multicast
```bash
# Check multicast support
ip maddr show

# Try binding to specific interface
# (may require code modification)
```

#### Segmentation Fault
```bash
# Build debug version
./build.sh debug

# Run with debugger
gdb ./build/matching_engine
(gdb) run --tcp
# ... crash ...
(gdb) backtrace
```

#### Memory Leaks
```bash
# Run with valgrind (Linux)
./build.sh valgrind

# Or manually
valgrind --leak-check=full ./build/matching_engine --tcp

# macOS: use leaks
leaks --atExit -- ./build/matching_engine --tcp
```

### Performance Issues

#### Low Throughput

**Check optimization level:**
```bash
# Should show -O3
cmake --build build --target info
```

**Verify processor mode:**
```bash
# Dual-processor should have better throughput
./build/matching_engine --tcp --dual-processor

# Check statistics on shutdown (Ctrl+C)
# Should show messages split across Processor 0 and Processor 1
```

#### High CPU Usage
```bash
# Check for busy-waiting in processor threads
# Review sleep parameters in processor.c

# Expected: 10-40% CPU in idle state
# High usage (>80%) may indicate busy-waiting bug
```

---

## Development Setup

### Recommended Setup

**IDE:** VS Code with C/C++ extension
```json
// .vscode/settings.json
{
    "C_Cpp.default.compilerPath": "/usr/bin/gcc",
    "C_Cpp.default.cStandard": "c11",
    "C_Cpp.default.includePath": [
        "${workspaceFolder}/include"
    ],
    "C_Cpp.default.defines": [
        "DEBUG"
    ]
}
```

**Build Task:** 
```json
// .vscode/tasks.json
{
    "version": "2.0.0",
    "tasks": [
        {
            "label": "build",
            "type": "shell",
            "command": "./build.sh",
            "args": ["build"],
            "group": {
                "kind": "build",
                "isDefault": true
            }
        },
        {
            "label": "debug",
            "type": "shell",
            "command": "./build.sh",
            "args": ["debug"]
        },
        {
            "label": "test",
            "type": "shell",
            "command": "./build.sh",
            "args": ["test"]
        }
    ]
}
```

**Launch Configuration:**
```json
// .vscode/launch.json
{
    "version": "0.2.0",
    "configurations": [
        {
            "name": "Debug Server",
            "type": "cppdbg",
            "request": "launch",
            "program": "${workspaceFolder}/build/matching_engine",
            "args": ["--tcp", "--dual-processor"],
            "cwd": "${workspaceFolder}",
            "preLaunchTask": "debug"
        },
        {
            "name": "Debug Server + Multicast",
            "type": "cppdbg",
            "request": "launch",
            "program": "${workspaceFolder}/build/matching_engine",
            "args": ["--tcp", "--multicast", "239.255.0.1:5000"],
            "cwd": "${workspaceFolder}",
            "preLaunchTask": "debug"
        }
    ]
}
```

### Code Formatting
```bash
# Format all source files
find src include -name '*.c' -o -name '*.h' | xargs clang-format -i

# Check formatting
clang-format --dry-run --Werror src/*.c
```

### Static Analysis
```bash
# Run cppcheck
cppcheck --enable=all --suppress=missingIncludeSystem src/

# Run clang static analyzer
scan-build cmake --build build
```

### Debugging
```bash
# Build with debug symbols
./build.sh debug

# Run with GDB
gdb ./build/matching_engine
(gdb) break main
(gdb) run --tcp --dual-processor
(gdb) next
(gdb) print variable_name
(gdb) backtrace
```

### Testing During Development
```bash
# Quick test cycle
./build.sh build && ./build.sh test

# Test specific processor mode
./build.sh build && ./build.sh test-dual-processor

# Test multicast
./build.sh build && ./build.sh test-multicast

# Full test suite
./build.sh test-all
```

---

## Build Output

### Successful Build
```
[STATUS] Configuring CMake...
  Build directory: build
  Build type: Release
  Generator: Ninja
  Platform: Linux
-- Configuring done
-- Generating done
-- Build files have been written to: build
[OK] Configuration complete

[STATUS] Building all targets...
[1/38] Building C object CMakeFiles/matching_engine_lib.dir/src/core/order_book.c.o
[2/38] Building C object CMakeFiles/matching_engine_lib.dir/src/core/matching_engine.c.o
...
[38/38] Linking C executable multicast_subscriber
[OK] Build complete
```

### Build Directory Structure
```
build/
├── matching_engine           # Main server executable
├── tcp_client               # TCP test client
├── binary_client            # Binary protocol client
├── binary_decoder           # Binary→CSV decoder
├── multicast_subscriber     # Multicast receiver tool
├── matching_engine_tests    # Unit tests
└── CMakeFiles/              # CMake build files
    └── matching_engine_lib.dir/
        └── src/
            ├── core/
            ├── protocol/
            ├── network/
            ├── threading/
            └── modes/       # Modular mode implementations
```

---

## Quick Reference

### Common Commands
```bash
# Full build
./build.sh build

# Build and test
./build.sh build && ./build.sh test

# Debug build
./build.sh debug

# Clean rebuild
./build.sh rebuild

# Check configuration
./build.sh info

# View all commands
./build.sh help
```

### Processor Mode Testing
```bash
# Test dual-processor routing
./build.sh test-dual-processor

# Test single-processor mode
./build.sh test-single-processor

# Run manually
./build/matching_engine --tcp --dual-processor
./build/matching_engine --tcp --single-processor
```

### Multicast Testing
```bash
# Interactive multicast test
./build.sh test-multicast

# Manual multicast testing
# Terminal 1:
./build/matching_engine --tcp --multicast 239.255.0.1:5000

# Terminal 2 (and 3, 4, ...):
./build/multicast_subscriber 239.255.0.1 5000

# Terminal N:
./build/tcp_client localhost 1234
```

### Build Time

On a modern system (i7/Ryzen, 16GB RAM):
- **Clean build:** ~5-10 seconds (Ninja)
- **Incremental build:** ~1-2 seconds
- **Full test suite:** ~5 seconds

---

## See Also

- [Quick Start Guide](QUICK_START.md) - Get running quickly
- [Architecture](ARCHITECTURE.md) - Dual-processor design, multicast, memory pools
- [Testing Guide](TESTING.md) - Testing including multicast tests
- [Protocols](PROTOCOLS.md) - Message formats and multicast protocol
