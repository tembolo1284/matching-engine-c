#!/bin/bash
# build.sh - Build script for Matching Engine
set -e

BUILD_DIR="build"
VALGRIND_BUILD_DIR="build-valgrind"
BUILD_TYPE="Release"
GENERATOR="Ninja"
DEFAULT_PORT=1234
MULTICAST_GROUP="239.255.0.1"
MULTICAST_PORT=5000

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
    if [ ! -x "$BUILD_DIR/matching_engine" ]; then
        print_error "matching_engine not built. Run ./build.sh build first."
        exit 1
    fi
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

mode_benchmark_udp() {
    require_built
    local port="${1:-$DEFAULT_PORT}"

    print_status "UDP Benchmark Mode (quiet - stats only)"
    echo ""
    echo "=========================================="
    echo "UDP Benchmark Mode"
    echo "=========================================="
    echo ""
    echo "Server suppresses per-message output for maximum throughput."
    echo "Progress printed every 10 seconds. Stats on shutdown (Ctrl+C)."
    echo ""
    echo "Terminal 1 (this terminal): Server in benchmark mode"
    echo ""
    echo "Terminal 2 (send orders):"
    echo "  ./build/matching_engine_client --scenario 23 --udp localhost ${port}"
    echo ""
    print_status "Starting UDP server in BENCHMARK mode. Ctrl+C for stats."

    "./${BUILD_DIR}/matching_engine" --udp "${port}" --quiet
}

mode_benchmark_tcp() {
    require_built
    local port="${1:-$DEFAULT_PORT}"

    print_status "TCP Benchmark Mode (quiet - stats only)"
    echo ""
    "./${BUILD_DIR}/matching_engine" --tcp "${port}" --quiet
}

mode_benchmark_matching() {
    require_client_built
    local scenario="${1:-24}"
    local port="${2:-$DEFAULT_PORT}"

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

    "./${BUILD_DIR}/matching_engine" --udp "$port" --quiet 2>&1 &
    local server_pid=$!
    sleep 1

    if ! kill -0 "$server_pid" 2>/dev/null; then
        print_error "Server failed to start"
        exit 1
    fi

    print_status "Server started in benchmark mode (PID: $server_pid)"
    echo ""

    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --udp localhost "$port"

    sleep 2

    print_status "Shutting down server to display statistics..."
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true

    print_success "Matching benchmark complete"
}

mode_benchmark_dual_processor() {
    require_client_built
    local scenario="${1:-26}"
    local port="${2:-$DEFAULT_PORT}"

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

    "./${BUILD_DIR}/matching_engine" --udp "$port" --quiet 2>&1 &
    local server_pid=$!
    sleep 1

    if ! kill -0 "$server_pid" 2>/dev/null; then
        print_error "Server failed to start"
        exit 1
    fi

    print_status "Server started in benchmark mode (PID: $server_pid)"
    echo ""

    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --udp localhost "$port"

    sleep 5

    print_status "Shutting down server to display statistics..."
    kill -TERM "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true

    print_success "Dual-processor benchmark complete"
}

# -------------------------
# Client Run Modes
# -------------------------

mode_client_interactive() {
    require_client_built
    local host="${1:-localhost}"
    local port="${2:-$DEFAULT_PORT}"

    print_status "Client Interactive Mode"
    echo ""
    echo "Ensure server is running:"
    echo "  ./${BUILD_DIR}/matching_engine --tcp ${port}"
    echo ""

    "./${BUILD_DIR}/matching_engine_client" "$host" "$port"
}

mode_client_scenario() {
    require_client_built
    local scenario="${1:-1}"
    local host="${2:-localhost}"
    local port="${3:-$DEFAULT_PORT}"

    print_status "Client Scenario Mode"
    echo ""
    echo "Running scenario ${scenario} against ${host}:${port}"
    echo ""

    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" "$host" "$port"
}

mode_client_udp() {
    require_client_built
    local scenario="${1:-1}"
    local host="${2:-localhost}"
    local port="${3:-$DEFAULT_PORT}"

    print_status "Client UDP Mode"
    echo ""
    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --udp "$host" "$port"
}

mode_client_tcp() {
    require_client_built
    local scenario="${1:-1}"
    local host="${2:-localhost}"
    local port="${3:-$DEFAULT_PORT}"

    print_status "Client TCP Mode"
    echo ""
    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --tcp "$host" "$port"
}

list_client_scenarios() {
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
        "./${VALGRIND_BUILD_DIR}/matching_engine" --udp "$DEFAULT_PORT" || true

    print_success "Valgrind server check complete"
}

# -------------------------
# Direct Server Run
# -------------------------

run_server() {
    require_built
    shift || true
    "./${BUILD_DIR}/matching_engine" "$@"
}

# -------------------------
# Help
# -------------------------

show_help() {
    cat << EOF
Usage:
  ./build.sh [command] [args...]

Build Commands:
  build              Build Release
  debug              Build Debug
  rebuild            Clean + rebuild
  clean              Remove build directory
  clean-all          Remove all build directories

Test Commands:
  test               Run unit tests
  valgrind           Run unit tests under valgrind
  valgrind-server    Run server under valgrind

Benchmark Modes (the real throughput tests):
  benchmark-udp      Start UDP server in quiet mode (manual client)
  benchmark-tcp      Start TCP server in quiet mode (manual client)
  benchmark-match N  Single-symbol matching (23=1M, 24=10M, 25=50M pairs)
  benchmark-dual N   Dual-processor matching (26=250M, 27=500M pairs)

Client Commands:
  client [host] [port]           Interactive client
  client-scenario N [host] [port] Run scenario N
  client-udp N [host] [port]     Run scenario via UDP
  client-tcp N [host] [port]     Run scenario via TCP
  scenarios                       List available scenarios

Server Commands:
  run-udp            Start UDP server
  run-tcp            Start TCP server
  run-quiet          Start UDP server in quiet mode

Available Scenarios:
  Basic (1-3):       Correctness testing
  Stress (10-12):    1K, 10K, 100K orders (non-matching, quick validation)
  Matching (20-25):  1K to 50M pairs (sustainable throughput)
  Dual-Proc (26-27): 250M, 500M pairs (ultimate test, both processors)

Examples:
  ./build.sh build
  ./build.sh test

  # Quick matching test (1M pairs = 2M orders):
  ./build.sh benchmark-match 23

  # Big matching test (50M pairs = 100M orders):
  ./build.sh benchmark-match 25

  # Ultimate test (250M pairs = 500M orders, dual processor):
  ./build.sh benchmark-dual 26

  # 1 BILLION orders (500M pairs):
  ./build.sh benchmark-dual 27

  # Manual two-terminal workflow:
  # Terminal 1: ./build.sh benchmark-udp
  # Terminal 2: ./build.sh client-udp 23
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

        # Benchmarks
        benchmark-udp)
            shift
            mode_benchmark_udp "$@"
            ;;
        benchmark-tcp)
            shift
            mode_benchmark_tcp "$@"
            ;;
        benchmark-match|benchmark-matching)
            shift
            mode_benchmark_matching "$@"
            ;;
        benchmark-dual)
            shift
            mode_benchmark_dual_processor "$@"
            ;;

        # Client
        client)
            shift
            mode_client_interactive "$@"
            ;;
        client-scenario)
            shift
            mode_client_scenario "$@"
            ;;
        client-udp)
            shift
            mode_client_udp "$@"
            ;;
        client-tcp)
            shift
            mode_client_tcp "$@"
            ;;
        scenarios|list-scenarios)
            list_client_scenarios
            ;;

        # Server
        run-udp)
            run_server --udp "$DEFAULT_PORT"
            ;;
        run-tcp)
            run_server --tcp "$DEFAULT_PORT"
            ;;
        run-quiet)
            run_server --udp "$DEFAULT_PORT" --quiet
            ;;
        run)
            run_server "$@"
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

