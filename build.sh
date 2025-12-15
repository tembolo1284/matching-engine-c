#!/bin/bash
# build.sh - Build script for Matching Engine
set -e

BUILD_DIR="build"
VALGRIND_BUILD_DIR="build-valgrind"
BUILD_TYPE="Release"
GENERATOR="Ninja"

# Fixed ports (unified server)
TCP_PORT=1234
UDP_PORT=1235
MULTICAST_PORT=1236
MULTICAST_GROUP="239.255.0.1"

# Detect platform
if [[ "$OSTYPE" == "darwin"* ]]; then
    PLATFORM="macOS"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    PLATFORM="Linux"
else
    PLATFORM="Unknown"
fi

print_status()  { echo "[STATUS] $1"; }
print_success() { echo "[OK] $1"; }
print_error()   { echo "[ERROR] $1"; }
print_warning() { echo "[WARN] $1"; }

command_exists() { command -v "$1" >/dev/null 2>&1; }

detect_generator() {
    if command_exists ninja; then
        echo "Ninja"
    else
        echo "Unix Makefiles"
    fi
}

configure() {
    local build_type=$1
    local generator=$2
    local build_dir=${3:-$BUILD_DIR}
    local extra_flags=${4:-}

    print_status "Configuring CMake..."
    echo "  Build directory: $build_dir"
    echo "  Build type: $build_type"
    echo "  Generator: $generator"
    echo "  Platform: $PLATFORM"
    if [ -n "$extra_flags" ]; then
        echo "  Extra flags: $extra_flags"
    fi

    cmake -B "$build_dir" -G "$generator" -DCMAKE_BUILD_TYPE="$build_type" $extra_flags
    print_success "Configuration complete"
}

build() {
    local build_dir=${1:-$BUILD_DIR}
    shift || true
    local targets=("$@")

    if [ ${#targets[@]} -eq 0 ]; then
        print_status "Building all targets..."
        cmake --build "$build_dir"
    else
        print_status "Building targets: ${targets[*]}..."
        for t in "${targets[@]}"; do
            cmake --build "$build_dir" --target "$t"
        done
    fi
    print_success "Build complete"
}

clean() {
    if [ -d "$BUILD_DIR" ]; then
        print_status "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
        rm -rf "$VALGRIND_BUILD_DIR"
        print_success "Clean complete"
    else
        print_warning "Build directory does not exist"
    fi
}

clean_valgrind() {
    if [ -d "$VALGRIND_BUILD_DIR" ]; then
        print_status "Cleaning valgrind build directory..."
        rm -rf "$VALGRIND_BUILD_DIR"
        print_success "Clean complete"
    else
        print_warning "Valgrind build directory does not exist"
    fi
}

require_built() {
    if [ ! -x "$BUILD_DIR/matching_engine" ]; then
        print_error "matching_engine not built. Run ./build.sh build first."
        exit 1
    fi
}

require_client_built() {
    require_built
    if [ ! -x "$BUILD_DIR/matching_engine_client" ]; then
        print_error "matching_engine_client not built. Run ./build.sh build first."
        exit 1
    fi
}

require_valgrind_built() {
    if [ ! -x "$VALGRIND_BUILD_DIR/matching_engine_tests" ]; then
        print_error "Valgrind-compatible build not found."
        print_status "Building valgrind-compatible version..."
        build_for_valgrind
    fi
}

build_for_valgrind() {
    print_status "Building valgrind-compatible version (no AVX-512)..."
    clean_valgrind
    configure "Debug" "$GENERATOR" "$VALGRIND_BUILD_DIR" "-DVALGRIND_BUILD=ON"
    build "$VALGRIND_BUILD_DIR"
    print_success "Valgrind build complete"
}

# -------------------------
# Server Run Modes
# -------------------------

run_server() {
    require_built
    print_status "Starting Unified Server"
    echo ""
    echo "=========================================="
    echo "  Matching Engine - Unified Server"
    echo "=========================================="
    echo "  TCP:       port $TCP_PORT"
    echo "  UDP:       port $UDP_PORT"
    echo "  Multicast: $MULTICAST_GROUP:$MULTICAST_PORT"
    echo "=========================================="
    echo ""
    "./${BUILD_DIR}/matching_engine" "$@"
}

run_quiet() {
    require_built
    print_status "Starting Unified Server (Quiet/Benchmark Mode)"
    echo ""
    "./${BUILD_DIR}/matching_engine" --quiet "$@"
}

run_binary() {
    require_built
    print_status "Starting Unified Server (Binary Format)"
    echo ""
    "./${BUILD_DIR}/matching_engine" --binary "$@"
}

run_benchmark() {
    require_built
    print_status "Starting Unified Server (Binary + Quiet)"
    echo ""
    "./${BUILD_DIR}/matching_engine" --binary --quiet "$@"
}

# -------------------------
# Benchmark Modes
# -------------------------

benchmark_matching() {
    require_client_built
    local scenario="${1:-24}"

    print_status "Matching Benchmark Test (Scenario ${scenario})"
    echo ""
    echo "=========================================="
    echo "Matching Benchmark - Sustained Throughput"
    echo "=========================================="
    echo ""
    echo "This test sends buy/sell pairs that match immediately,"
    echo "testing sustained throughput without pool exhaustion."
    echo ""
    echo "Available matching scenarios:"
    echo "  23 - 1M pairs   (2M orders)    ~5 sec"
    echo "  24 - 10M pairs  (20M orders)   ~45 sec"
    echo "  25 - 50M pairs  (100M orders)  ~4 min"
    echo ""

    "./${BUILD_DIR}/matching_engine" --quiet 2>&1 &
    local server_pid=$!
    sleep 1

    if ! kill -0 "$server_pid" 2>/dev/null; then
        print_error "Server failed to start"
        exit 1
    fi

    print_status "Server started (PID: $server_pid)"
    echo ""

    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --udp localhost "$UDP_PORT"

    sleep 2

    print_status "Shutting down server to display statistics..."
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true

    print_success "Matching benchmark complete"
}

benchmark_dual() {
    require_client_built
    local scenario="${1:-26}"

    print_status "DUAL-PROCESSOR Matching Benchmark (Scenario ${scenario})"
    echo ""
    echo "============================================================"
    echo "  DUAL-PROCESSOR BENCHMARK"
    echo "============================================================"
    echo ""
    echo "This test sends matching pairs across 10 symbols,"
    echo "exercising BOTH processors simultaneously."
    echo ""
    echo "Available dual-processor scenarios:"
    echo "  26 - 250M pairs (500M orders)  ~15-20 min"
    echo "  27 - 500M pairs (1B orders)    ~30-40 min"
    echo ""

    "./${BUILD_DIR}/matching_engine" --quiet 2>&1 &
    local server_pid=$!
    sleep 1

    if ! kill -0 "$server_pid" 2>/dev/null; then
        print_error "Server failed to start"
        exit 1
    fi

    print_status "Server started (PID: $server_pid)"
    echo ""

    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --udp localhost "$UDP_PORT"

    sleep 5

    print_status "Shutting down server to display statistics..."
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true

    print_success "Dual-processor benchmark complete"
}

# -------------------------
# Client Run Modes
# -------------------------

client_interactive() {
    require_client_built
    local transport="${1:-tcp}"
    local host="localhost"
    local port="$TCP_PORT"

    if [ "$transport" = "udp" ]; then
        port="$UDP_PORT"
    fi

    print_status "Client Interactive Mode ($transport)"
    echo ""
    echo "Ensure server is running: ./build.sh run"
    echo ""

    if [ "$transport" = "udp" ]; then
        "./${BUILD_DIR}/matching_engine_client" --udp "$host" "$port"
    else
        "./${BUILD_DIR}/matching_engine_client" "$host" "$port"
    fi
}

client_scenario() {
    require_client_built
    local scenario="${1:-1}"
    local transport="${2:-tcp}"
    local host="localhost"
    local port="$TCP_PORT"

    if [ "$transport" = "udp" ]; then
        port="$UDP_PORT"
    fi

    print_status "Client Scenario Mode (scenario $scenario, $transport)"
    echo ""

    if [ "$transport" = "udp" ]; then
        "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --udp "$host" "$port"
    else
        "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" "$host" "$port"
    fi
}

list_scenarios() {
    require_client_built
    "./${BUILD_DIR}/matching_engine_client" --list-scenarios
}

# -------------------------
# Test Modes
# -------------------------

run_unit_tests() {
    print_status "Running unit tests..."
    cmake --build "$BUILD_DIR" --target test-unit
    print_success "Unit tests complete"
}

run_valgrind() {
    if [ "$PLATFORM" != "Linux" ]; then
        print_error "valgrind only supported on Linux"
        exit 1
    fi

    if ! command_exists valgrind; then
        print_error "valgrind not found. Install with: sudo apt install valgrind"
        exit 1
    fi

    require_valgrind_built

    print_status "Running valgrind on unit tests..."
    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
        --error-exitcode=1 \
        "./${VALGRIND_BUILD_DIR}/matching_engine_tests"

    print_success "Valgrind memory check passed!"
}

run_valgrind_server() {
    if [ "$PLATFORM" != "Linux" ]; then
        print_error "valgrind only supported on Linux"
        exit 1
    fi

    require_valgrind_built

    local timeout_secs="${1:-5}"
    print_status "Running valgrind on server for ${timeout_secs} seconds..."

    timeout --signal=SIGINT "${timeout_secs}" \
        valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
        "./${VALGRIND_BUILD_DIR}/matching_engine" || true

    print_success "Valgrind server check complete"
}

# -------------------------
# Help
# -------------------------

show_help() {
    cat << EOF
Matching Engine - Build Script
==============================

Usage: ./build.sh [command] [args...]

BUILD COMMANDS
  build              Build Release
  debug              Build Debug
  rebuild            Clean + rebuild
  clean              Remove build directory
  clean-all          Remove all build directories

TEST COMMANDS
  test               Run unit tests
  valgrind           Run unit tests under valgrind
  valgrind-server    Run server under valgrind

SERVER COMMANDS (Unified - all transports always active)
  run                Start server (CSV, dual processor)
  run-binary         Start server (binary format)
  run-quiet          Start server (quiet/benchmark mode)
  run-benchmark      Start server (binary + quiet)

  Server always listens on:
    TCP:       $TCP_PORT
    UDP:       $UDP_PORT
    Multicast: $MULTICAST_GROUP:$MULTICAST_PORT

CLIENT COMMANDS
  client [tcp|udp]           Interactive client
  client-scenario N [tcp|udp] Run scenario N
  scenarios                   List available scenarios

BENCHMARK COMMANDS
  benchmark-match N   Matching benchmark (23=1M, 24=10M, 25=50M pairs)
  benchmark-dual N    Dual-processor benchmark (26=250M, 27=500M pairs)

SCENARIOS
  Basic (1-3):       Correctness testing
  Stress (10-12):    1K, 10K, 100K orders
  Matching (20-25):  1K to 50M pairs (sustainable throughput)
  Dual-Proc (26-27): 250M, 500M pairs (ultimate test)

EXAMPLES
  ./build.sh build                  # Build everything
  ./build.sh test                   # Run unit tests

  ./build.sh run                    # Start server (CSV)
  ./build.sh run-binary             # Start server (binary)
  ./build.sh run-quiet              # Start server (benchmark mode)

  ./build.sh client                 # Interactive TCP client
  ./build.sh client udp             # Interactive UDP client
  ./build.sh client-scenario 1      # Run scenario 1 via TCP
  ./build.sh client-scenario 23 udp # Run scenario 23 via UDP

  ./build.sh benchmark-match 23     # Quick test: 1M pairs
  ./build.sh benchmark-match 25     # Big test: 50M pairs
  ./build.sh benchmark-dual 26      # Ultimate: 250M pairs
EOF
}

# Normalize --flags
case "$1" in
    --clean) set -- "clean" ;;
    --help|-h) set -- "help" ;;
    --build) set -- "build" ;;
    --test) set -- "test" ;;
esac

main() {
    if [ $# -eq 0 ]; then
        show_help
        exit 0
    fi

    GENERATOR=$(detect_generator)

    case "$1" in
        # Build
        build)
            BUILD_TYPE="Release"
            clean
            configure "$BUILD_TYPE" "$GENERATOR"
            build
            ;;
        debug)
            BUILD_TYPE="Debug"
            clean
            configure "$BUILD_TYPE" "$GENERATOR"
            build
            ;;
        rebuild)
            clean
            configure "$BUILD_TYPE" "$GENERATOR"
            build
            ;;
        clean)
            clean
            ;;
        clean-all)
            clean
            clean_valgrind
            ;;

        # Tests
        test)
            run_unit_tests
            ;;
        valgrind)
            run_valgrind
            ;;
        valgrind-server)
            shift
            run_valgrind_server "$@"
            ;;

        # Server
        run)
            shift
            run_server "$@"
            ;;
        run-binary)
            shift
            run_binary "$@"
            ;;
        run-quiet)
            shift
            run_quiet "$@"
            ;;
        run-benchmark)
            shift
            run_benchmark "$@"
            ;;

        # Benchmarks
        benchmark-match|benchmark-matching)
            shift
            benchmark_matching "$@"
            ;;
        benchmark-dual)
            shift
            benchmark_dual "$@"
            ;;

        # Client
        client)
            shift
            client_interactive "$@"
            ;;
        client-scenario)
            shift
            client_scenario "$@"
            ;;
        scenarios|list-scenarios)
            list_scenarios
            ;;

        # Help
        help)
            show_help
            ;;
        *)
            print_error "Unknown command: $1"
            echo "Run ./build.sh help for usage"
            exit 1
            ;;
    esac
}

if ! command_exists cmake; then
    print_error "CMake not found"
    exit 1
fi

main "$@"
