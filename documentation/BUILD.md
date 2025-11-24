# Build Instructions

Detailed guide for building the Matching Engine on various platforms.

## Table of Contents
- [Prerequisites](#prerequisites)
- [Quick Build](#quick-build)
- [Build Systems](#build-systems)
- [Build Targets](#build-targets)
- [Build Options](#build-options)
- [Platform-Specific Notes](#platform-specific-notes)
- [Troubleshooting](#troubleshooting)
- [Development Setup](#development-setup)

---

## Prerequisites

### Required Tools

| Tool | Minimum Version | Purpose |
|------|----------------|---------|
| GCC | 4.9+ | C11 support (`<stdatomic.h>`) |
| GNU Make | 3.81+ | Build orchestration |
| POSIX System | - | pthreads, sockets |

### Optional Tools

| Tool | Purpose |
|------|---------|
| CMake 3.10+ | Alternative build system |
| Ninja | Fast CMake backend |
| Valgrind | Memory leak detection |
| GDB | Debugging |
| clang-format | Code formatting |

---

## Quick Build

### Default Build (Make)

```bash
# Clean previous build
make clean

# Build everything
make

# Build artifacts in build/ directory:
#   - matching_engine      (main server)
#   - tcp_client          (TCP test client)
#   - binary_client       (binary protocol client)
#   - binary_decoder      (binary→CSV decoder)
#   - matching_engine_tests (unit tests)
```

### Alternative Build (CMake + Ninja)

```bash
# Generate build files
cmake -B build -G Ninja

# Build
cmake --build build

# Build artifacts in build/ directory (same as Make)
```

---

## Build Systems

### Makefile (Recommended)

**Advantages:**
- Simple and widely available
- No additional dependencies
- Clear build output
- Custom targets for testing

**Structure:**
```makefile
CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -O3 -pthread
INCLUDES = -Iinclude
SRC_DIR = src
BUILD_DIR = build

# Automatic dependency tracking
DEPS = $(OBJS:.o=.d)
-include $(DEPS)
```

**Build process:**
1. Compiles each `.c` file to `.o` in `build/obj/`
2. Generates dependency files (`.d`) automatically
3. Links object files into executables
4. Places executables in `build/`

### CMake (Alternative)

**Advantages:**
- Platform-independent
- IDE integration (CLion, VS Code)
- Better dependency management
- Modern build tool

**Usage:**
```bash
# Configure (first time)
cmake -B build -G Ninja

# Reconfigure (after CMakeLists.txt changes)
cmake -B build

# Build
cmake --build build

# Clean
cmake --build build --target clean
```

**CMakeLists.txt features:**
- Automatic source file discovery
- Compiler feature detection
- Build type configuration (Debug/Release)

---

## Build Targets

### Main Targets (Make)

```bash
# Build main server only
make matching_engine

# Build TCP client only
make tcp_client

# Build binary tools only
make binary-tools

# Build tests only
make tests

# Build everything (default)
make
```

### Special Targets

```bash
# Clean build artifacts
make clean

# Debug build (with -g -O0)
make debug

# View build configuration
make info

# Run tests
make test

# Run TCP integration test
make test-tcp

# Run binary protocol tests
make test-binary
make test-binary-full

# Run all tests
make test-all

# Memory leak check
make valgrind-test
```

### CMake Targets

```bash
# Build specific target
cmake --build build --target matching_engine
cmake --build build --target tcp_client
cmake --build build --target binary_client

# Clean
cmake --build build --target clean
```

---

## Build Options

### Compiler Flags

#### Default (Optimized)
```makefile
CFLAGS = -std=c11 -Wall -Wextra -O3 -pthread
```

- `-std=c11` - C11 standard (required for atomics)
- `-Wall -Wextra` - Enable all warnings
- `-O3` - Maximum optimization
- `-pthread` - POSIX threads support

#### Debug Build
```bash
make debug
# Uses: -std=c11 -Wall -Wextra -g -O0 -pthread
```

- `-g` - Debug symbols for GDB
- `-O0` - No optimization (easier debugging)

#### Custom Flags
```bash
# Override compiler
make CC=clang

# Add custom flags
make CFLAGS="-std=c11 -O2 -march=native"

# Debug with sanitizers
make CFLAGS="-std=c11 -g -O0 -fsanitize=address -fsanitize=undefined"
```

### Build Modes

#### Release (default)
- Maximum optimization (`-O3`)
- No debug symbols
- Best performance
- Use for production

```bash
make
```

#### Debug
- No optimization (`-O0`)
- Debug symbols (`-g`)
- Easier debugging
- Use for development

```bash
make debug
```

#### With Sanitizers
- Address Sanitizer (memory errors)
- Undefined Behavior Sanitizer
- Use for testing

```bash
make CFLAGS="-std=c11 -g -O0 -fsanitize=address -fsanitize=undefined"
./build/matching_engine --tcp
```

---

## Platform-Specific Notes

### Linux (Ubuntu/Debian)

#### Install Prerequisites
```bash
# GCC and Make
sudo apt-get update
sudo apt-get install build-essential

# Optional: CMake and Ninja
sudo apt-get install cmake ninja-build

# Optional: Valgrind
sudo apt-get install valgrind

# Verify installation
gcc --version    # Should show 4.9+
make --version   # Should show 3.81+
```

#### Build
```bash
# Standard build
make clean && make

# Everything works out of the box
```

#### Known Issues
- **None** - Linux is the primary development platform

### macOS

#### Install Prerequisites
```bash
# Install Xcode Command Line Tools
xcode-select --install

# Or install via Homebrew
brew install gcc make cmake ninja

# Verify installation
gcc --version
```

#### Build
```bash
# Standard build
make clean && make

# Note: macOS uses Clang by default (works fine)
```

#### Known Issues

**Issue 1: Clang vs GCC**
- macOS `gcc` is often aliased to `clang`
- This usually works fine (Clang has good C11 support)
- To force real GCC:
```bash
brew install gcc
make CC=gcc-13  # Or whatever version Homebrew installed
```

**Issue 2: Network buffer sizes**
- macOS has smaller default network buffers
- May see dropped UDP packets under high load
- Workaround: Use TCP mode or increase buffer sizes

```c
// In udp_receiver.c
int buffer_size = 10 * 1024 * 1024;  // 10MB
setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &buffer_size, sizeof(buffer_size));
```

### Other POSIX Systems (FreeBSD, etc.)

#### Prerequisites
```bash
# Install via package manager
pkg install gcc make cmake ninja  # FreeBSD
```

#### Build
```bash
make clean && make
```

#### Known Issues
- May need to adjust `#include <sys/socket.h>` headers
- Check `man pthread` for POSIX compliance
- Network constants may differ (`SOL_SOCKET`, etc.)

### Windows (WSL/Cygwin/MinGW)

#### Windows Subsystem for Linux (Recommended)
```bash
# In WSL Ubuntu
sudo apt-get install build-essential
make clean && make
# Works exactly like Linux
```

#### Cygwin
```bash
# Install gcc-core, make packages via Cygwin installer
make clean && make
# Should work with minor adjustments
```

#### Native Windows (MinGW)
```bash
# Install MinGW-w64
# Adjust Makefile for Windows paths and libraries
# May require Winsock adaptations
```

**Known Issues:**
- Native Windows requires Winsock2 instead of POSIX sockets
- POSIX threads not available (use WinAPI threads)
- Significant porting effort required
- **Recommendation:** Use WSL instead

---

## Troubleshooting

### Build Errors

#### Error: "undefined reference to pthread_create"
```
Linking build/matching_engine...
/usr/bin/ld: ... undefined reference to `pthread_create'
```

**Solution:** Missing `-pthread` flag
```bash
# Check CFLAGS includes -pthread
make info

# Force rebuild
make clean && make
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

# Use newer GCC
make CC=gcc-11
```

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

### Link Errors

#### Error: "multiple definition of ..."
```
build/obj/main.o: multiple definition of `order_book_create'
```

**Solution:** Function defined in header (should be declared only)
```c
// In .h file (WRONG)
void order_book_create() { ... }

// In .h file (CORRECT)
void order_book_create();

// In .c file
void order_book_create() { ... }
```

### Runtime Errors

#### Segmentation Fault
```bash
# Run with debugger
gdb ./build/matching_engine
(gdb) run --tcp
# ... crash ...
(gdb) backtrace
```

#### Memory Leaks
```bash
# Run with valgrind
valgrind --leak-check=full ./build/matching_engine --tcp

# Or use make target
make valgrind-test
```

### Performance Issues

#### Low Throughput
```bash
# Verify optimization level
make info  # Should show -O3

# Rebuild with optimizations
make clean
make CFLAGS="-std=c11 -O3 -march=native -pthread"
```

#### High CPU Usage
```bash
# Check for busy-waiting
# Review sleep parameters in processor.c
```

---

## Development Setup

### Recommended Setup

**Editor:** VS Code with C/C++ extension
```json
// .vscode/settings.json
{
    "C_Cpp.default.compilerPath": "/usr/bin/gcc",
    "C_Cpp.default.cStandard": "c11",
    "C_Cpp.default.includePath": [
        "${workspaceFolder}/include"
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
            "command": "make",
            "group": {
                "kind": "build",
                "isDefault": true
            }
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
scan-build make
```

### Debugging

```bash
# Build with debug symbols
make debug

# Run with GDB
gdb ./build/matching_engine
(gdb) break main
(gdb) run --tcp
(gdb) next
(gdb) print variable_name
```

### Profiling

```bash
# Build with profiling
make CFLAGS="-std=c11 -O3 -pg -pthread"

# Run to generate gmon.out
./build/matching_engine --tcp
# ... run tests ...
# Ctrl+C

# Analyze
gprof ./build/matching_engine gmon.out
```

---

## Build Output

### Successful Build

```
Compiling src/message_parser.c...
Compiling src/message_formatter.c...
Compiling src/binary_message_parser.c...
Compiling src/binary_message_formatter.c...
Compiling src/order_book.c...
Compiling src/matching_engine.c...
Compiling src/udp_receiver.c...
Compiling src/tcp_listener.c...
Compiling src/processor.c...
Compiling src/output_publisher.c...
Compiling src/main.c...
Linking build/matching_engine...

Building TCP client...
Compiling tools/tcp_client.c...
Linking build/tcp_client...

Building binary client...
Compiling tools/binary_client.c...
Linking build/binary_client...

Building binary decoder...
Compiling tools/binary_decoder.c...
Linking build/binary_decoder...

Building tests...
Compiling tests/test_runner.c...
Compiling tests/test_order_book.c...
Compiling tests/test_message_parser.c...
Compiling tests/test_message_formatter.c...
Compiling tests/test_matching_engine.c...
Compiling tests/test_scenarios_odd.c...
Compiling tests/test_scenarios_even.c...
Linking build/matching_engine_tests...

✓ Build complete!
  Main executable: build/matching_engine
  Test executable: build/matching_engine_tests
  Client tools:    build/tcp_client, build/binary_client, build/binary_decoder
```

### Build Directory Structure

```
build/
├── matching_engine           # Main server executable
├── tcp_client               # TCP test client
├── binary_client            # Binary protocol client
├── binary_decoder           # Binary→CSV decoder
├── matching_engine_tests    # Unit tests
└── obj/                     # Object files
    ├── main.o
    ├── order_book.o
    ├── matching_engine.o
    ├── message_parser.o
    ├── message_formatter.o
    ├── binary_message_parser.o
    ├── binary_message_formatter.o
    ├── udp_receiver.o
    ├── tcp_listener.o
    ├── processor.o
    ├── output_publisher.o
    └── tests/               # Test object files
        ├── test_runner.o
        ├── test_order_book.o
        └── ...
```

---

## Advanced Build Options

### Cross-Compilation

```bash
# ARM cross-compile
make CC=arm-linux-gnueabi-gcc

# MIPS cross-compile
make CC=mips-linux-gnu-gcc
```

### Static Linking

```bash
# Build fully static binary
make LDFLAGS="-static"
```

### Profile-Guided Optimization

```bash
# Step 1: Build with profiling
make CFLAGS="-std=c11 -O3 -fprofile-generate -pthread"

# Step 2: Run typical workload
./build/matching_engine --tcp
# ... run representative tests ...
# Ctrl+C

# Step 3: Rebuild with profile data
make clean
make CFLAGS="-std=c11 -O3 -fprofile-use -pthread"
```

---

## Continuous Integration

### GitHub Actions Example

```yaml
name: Build and Test
on: [push, pull_request]

jobs:
  build-linux:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v2
      
      - name: Install dependencies
        run: sudo apt-get install build-essential valgrind
      
      - name: Build
        run: make clean && make
      
      - name: Run tests
        run: make test-all
      
      - name: Memory check
        run: make valgrind-test
      
      - name: Upload artifacts
        uses: actions/upload-artifact@v2
        with:
          name: matching-engine
          path: build/matching_engine

  build-macos:
    runs-on: macos-latest
    steps:
      - uses: actions/checkout@v2
      
      - name: Build
        run: make clean && make
      
      - name: Run tests
        run: make test-all
```

---

## Quick Reference

### Common Commands

```bash
# Full clean build
make clean && make

# Build and test
make clean && make test-all

# Debug build
make debug

# Check build config
make info

# View all targets
make help
```

### Build Time

On a modern system (i7/Ryzen, 16GB RAM):
- **Clean build:** ~5-10 seconds
- **Incremental build:** ~1-2 seconds
- **Full test suite:** ~5 seconds

---

## See Also

- [Quick Start Guide](QUICK_START.md) - Get running quickly
- [Architecture](ARCHITECTURE.md) - System design
- [Testing Guide](TESTING.md) - Running tests
- [Protocols](PROTOCOLS.md) - Message formats
