#!/bin/bash
# build.sh - Build script for Matching Engine (README run-modes)
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
    if [ ! -x "$BUILD_DIR/binary_client" ]; then
        print_error "binary_client not built. Run ./build.sh build first."
        exit 1
    fi
}
require_multicast_built() {
    if [ ! -x "$BUILD_DIR/matching_engine" ]; then
        print_error "matching_engine not built. Run ./build.sh build first."
        exit 1
    fi
    if [ ! -x "$BUILD_DIR/multicast_subscriber" ]; then
        print_error "multicast_subscriber not built. Run ./build.sh build first."
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
require_decoder_built() {
    if [ ! -x "$BUILD_DIR/binary_decoder" ]; then
        print_error "binary_decoder not built. Run ./build.sh build first."
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
# scenarios args: numbers or "all" (default 1 2 3)
parse_scenarios() {
    local scenarios=()
    while [[ $# -gt 0 ]]; do
        case "$1" in
            all)
                scenarios=(1 2 3)
                shift
                ;;
            [0-9]*)
                scenarios+=("$1")
                shift
                ;;
            *)
                break
                ;;
        esac
    done
    if [ ${#scenarios[@]} -eq 0 ]; then
        scenarios=(1 2 3)
    fi
    echo "${scenarios[@]}"
}
# -------------------------
# README run-modes (Server)
# -------------------------
mode_test_binary() {
    require_built
    local port="${1:-$DEFAULT_PORT}"
    shift || true
    local scenarios
    scenarios=($(parse_scenarios "$@"))
    print_status "README mode: test-binary"
    echo ""
    echo "Terminal 1 (this terminal):"
    echo "  ./${BUILD_DIR}/matching_engine --udp ${port}"
    echo ""
    echo "Terminal 2 (client side):"
    for s in "${scenarios[@]}"; do
        echo "  ./${BUILD_DIR}/binary_client ${port} ${s}"
    done
    echo ""
    print_status "Starting UDP server now (CSV output). Ctrl+C to stop."
    "./${BUILD_DIR}/matching_engine" --udp "${port}"
}
mode_test_binary_full() {
    require_built
    require_decoder_built
    local port="${1:-$DEFAULT_PORT}"
    shift || true
    local scenarios
    scenarios=($(parse_scenarios "$@"))
    print_status "README mode: test-binary-full"
    echo ""
    echo "Terminal 1 (this terminal):"
    echo "  ./${BUILD_DIR}/matching_engine --udp --binary ${port} 2>/dev/null | ./${BUILD_DIR}/binary_decoder"
    echo ""
    echo "Terminal 2 (client side):"
    for s in "${scenarios[@]}"; do
        echo "  ./${BUILD_DIR}/binary_client ${port} ${s}"
    done
    echo ""
    print_status "Starting UDP server now (binary output -> decoder). Ctrl+C to stop."
    "./${BUILD_DIR}/matching_engine" --udp --binary "${port}" 2>/dev/null | "./${BUILD_DIR}/binary_decoder"
}
mode_test_tcp() {
    require_built
    local port="${1:-$DEFAULT_PORT}"
    shift || true
    local scenarios
    scenarios=($(parse_scenarios "$@"))
    print_status "README mode: test-tcp"
    echo ""
    echo "Terminal 1 (this terminal):"
    echo "  ./${BUILD_DIR}/matching_engine --tcp ${port}"
    echo ""
    echo "Terminal 2 (client side):"
    for s in "${scenarios[@]}"; do
        echo "  ./${BUILD_DIR}/binary_client ${port} ${s} --tcp"
    done
    echo ""
    print_status "Starting TCP server now (CSV output). Ctrl+C to stop."
    "./${BUILD_DIR}/matching_engine" --tcp "${port}"
}
mode_test_tcp_csv() {
    require_built
    local port="${1:-$DEFAULT_PORT}"
    shift || true
    local scenarios
    scenarios=($(parse_scenarios "$@"))
    print_status "README mode: test-tcp-csv"
    echo ""
    echo "Terminal 1 (this terminal):"
    echo "  ./${BUILD_DIR}/matching_engine --tcp ${port}"
    echo ""
    echo "Terminal 2 (client side):"
    for s in "${scenarios[@]}"; do
        echo "  ./${BUILD_DIR}/binary_client ${port} ${s} --tcp --csv"
    done
    echo ""
    print_status "Starting TCP server now (CSV output). Ctrl+C to stop."
    "./${BUILD_DIR}/matching_engine" --tcp "${port}"
}
mode_test_dual_processor() {
    require_built
    local port="${1:-$DEFAULT_PORT}"
    print_status "Dual-Processor Symbol Routing Test"
    echo ""
    echo "=========================================="
    echo "Dual-Processor Symbol Routing Test"
    echo "=========================================="
    echo ""
    echo "This test verifies symbol-based routing:"
    echo "  - Symbols A-M → Processor 0"
    echo "  - Symbols N-Z → Processor 1"
    echo ""
    echo "Terminal 1 (this terminal):"
    echo "  Server starts in dual-processor mode (default)"
    echo ""
    echo "Terminal 2 (run these commands):"
    echo "  ./${BUILD_DIR}/tcp_client localhost ${port}"
    echo "  > buy IBM 100 50 1      # → Processor 0 (I is A-M)"
    echo "  > buy NVDA 200 25 2     # → Processor 1 (N is N-Z)"
    echo ""
    print_status "Starting TCP server in DUAL-PROCESSOR mode. Ctrl+C to stop."
    "./${BUILD_DIR}/matching_engine" --tcp "${port}" --dual-processor
}
mode_test_single_processor() {
    require_built
    local port="${1:-$DEFAULT_PORT}"
    print_status "Single-Processor Mode Test"
    echo ""
    echo "=========================================="
    echo "Single-Processor Mode Test"
    echo "=========================================="
    echo ""
    echo "All symbols route to a single processor."
    echo ""
    print_status "Starting TCP server in SINGLE-PROCESSOR mode. Ctrl+C to stop."
    "./${BUILD_DIR}/matching_engine" --tcp "${port}" --single-processor
}
mode_test_multicast() {
    require_multicast_built
    local port="${1:-$DEFAULT_PORT}"
    local mcast_group="${2:-$MULTICAST_GROUP}"
    local mcast_port="${3:-$MULTICAST_PORT}"
    print_status "Multicast Market Data Feed Test"
    echo ""
    echo "=========================================="
    echo "Multicast Market Data Feed Test"
    echo "=========================================="
    echo ""
    echo "Terminal 1 (this terminal): Server with multicast"
    echo ""
    echo "Terminal 2 (subscriber):"
    echo "  ./${BUILD_DIR}/multicast_subscriber ${mcast_group} ${mcast_port}"
    echo ""
    echo "Terminal 3 (send orders):"
    echo "  ./${BUILD_DIR}/tcp_client localhost ${port}"
    echo ""
    print_status "Starting TCP server with MULTICAST feed. Ctrl+C to stop."
    "./${BUILD_DIR}/matching_engine" --tcp "${port}" --multicast "${mcast_group}:${mcast_port}"
}
# -------------------------
# Client run-modes
# -------------------------
mode_client_interactive() {
    require_client_built
    local host="${1:-localhost}"
    local port="${2:-$DEFAULT_PORT}"
    print_status "Client Interactive Mode"
    echo ""
    echo "=========================================="
    echo "Matching Engine Client - Interactive"
    echo "=========================================="
    echo ""
    echo "Ensure server is running:"
    echo "  ./${BUILD_DIR}/matching_engine --tcp ${port}"
    echo ""
    echo "Commands: buy, sell, cancel, flush, scenario, stats, help, quit"
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
mode_client_stress() {
    require_client_built
    local scenario="${1:-11}"
    local host="${2:-localhost}"
    local port="${3:-$DEFAULT_PORT}"
    print_status "Client Stress Test"
    echo ""
    echo "Running stress scenario ${scenario} against ${host}:${port}"
    echo ""
    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" "$host" "$port"
}
mode_client_burst() {
    require_client_built
    local scenario="${1:-40}"
    local host="${2:-localhost}"
    local port="${3:-$DEFAULT_PORT}"
    print_status "Client BURST Mode (DANGER!)"
    echo ""
    echo "!!! WARNING: No throttling - may cause server buffer overflow !!!"
    echo ""
    echo "Running burst scenario ${scenario} against ${host}:${port}"
    echo ""
    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --danger-burst "$host" "$port"
}
mode_client_multicast() {
    require_client_built
    local host="${1:-localhost}"
    local port="${2:-$DEFAULT_PORT}"
    local mcast_group="${3:-$MULTICAST_GROUP}"
    local mcast_port="${4:-$MULTICAST_PORT}"
    print_status "Client with Multicast Subscription"
    echo ""
    echo "=========================================="
    echo "Client + Multicast Market Data"
    echo "=========================================="
    echo ""
    echo "Ensure server is running with multicast:"
    echo "  ./${BUILD_DIR}/matching_engine --tcp ${port} --multicast ${mcast_group}:${mcast_port}"
    echo ""
    "./${BUILD_DIR}/matching_engine_client" --multicast "${mcast_group}:${mcast_port}" "$host" "$port"
}
mode_client_mcast_only() {
    require_client_built
    local mcast_group="${1:-$MULTICAST_GROUP}"
    local mcast_port="${2:-$MULTICAST_PORT}"
    print_status "Multicast-Only Subscriber"
    echo ""
    echo "=========================================="
    echo "Multicast-Only Market Data Subscriber"
    echo "=========================================="
    echo ""
    echo "Ensure server is running with multicast:"
    echo "  ./${BUILD_DIR}/matching_engine --tcp ${DEFAULT_PORT} --multicast ${mcast_group}:${mcast_port}"
    echo ""
    echo "Press Ctrl+C to stop."
    echo ""
    "./${BUILD_DIR}/matching_engine_client" --multicast-only --multicast "${mcast_group}:${mcast_port}"
}

# -------------------------
# Client UDP/TCP/Binary modes
# -------------------------
mode_client_udp() {
    require_client_built
    local scenario="${1:-1}"
    local host="${2:-localhost}"
    local port="${3:-$DEFAULT_PORT}"
    
    print_status "Client UDP Mode (CSV encoding)"
    echo ""
    echo "Running scenario ${scenario} against ${host}:${port} via UDP"
    echo ""
    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --udp "$host" "$port"
}

mode_client_udp_binary() {
    require_client_built
    local scenario="${1:-1}"
    local host="${2:-localhost}"
    local port="${3:-$DEFAULT_PORT}"
    
    print_status "Client UDP Mode (Binary encoding)"
    echo ""
    echo "Running scenario ${scenario} against ${host}:${port} via UDP with binary encoding"
    echo ""
    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --udp --binary "$host" "$port"
}

mode_client_tcp() {
    require_client_built
    local scenario="${1:-1}"
    local host="${2:-localhost}"
    local port="${3:-$DEFAULT_PORT}"
    
    print_status "Client TCP Mode (CSV encoding)"
    echo ""
    echo "Running scenario ${scenario} against ${host}:${port} via TCP"
    echo ""
    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --tcp "$host" "$port"
}

mode_client_tcp_binary() {
    require_client_built
    local scenario="${1:-1}"
    local host="${2:-localhost}"
    local port="${3:-$DEFAULT_PORT}"
    
    print_status "Client TCP Mode (Binary encoding)"
    echo ""
    echo "Running scenario ${scenario} against ${host}:${port} via TCP with binary encoding"
    echo ""
    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --tcp --binary "$host" "$port"
}

# -------------------------
# Client test modes
# -------------------------
mode_test_client_basic() {
    require_client_built
    local port="${1:-$DEFAULT_PORT}"
    print_status "Client Basic Scenarios Test"
    echo ""
    echo "=========================================="
    echo "Running Client Scenarios 1, 2, 3"
    echo "=========================================="
    echo ""
    # Start server in background
    "./${BUILD_DIR}/matching_engine" --tcp "$port" > /tmp/server_output.txt 2>&1 &
    local server_pid=$!
    sleep 1
    if ! kill -0 "$server_pid" 2>/dev/null; then
        print_error "Server failed to start"
        cat /tmp/server_output.txt
        exit 1
    fi
    print_status "Server started (PID: $server_pid)"
    "./${BUILD_DIR}/matching_engine_client" --scenario 1 localhost "$port"
    echo ""
    "./${BUILD_DIR}/matching_engine_client" --scenario 2 localhost "$port"
    echo ""
    "./${BUILD_DIR}/matching_engine_client" --scenario 3 localhost "$port"
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    print_success "Client basic tests complete"
}
mode_test_client_stress() {
    require_client_built
    local scenario="${1:-11}"
    local port="${2:-$DEFAULT_PORT}"
    print_status "Client Stress Test (Scenario ${scenario})"
    echo ""
    "./${BUILD_DIR}/matching_engine" --tcp "$port" > /tmp/server_output.txt 2>&1 &
    local server_pid=$!
    sleep 1
    if ! kill -0 "$server_pid" 2>/dev/null; then
        print_error "Server failed to start"
        exit 1
    fi
    print_status "Server started (PID: $server_pid)"
    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" localhost "$port"
    kill -TERM "$server_pid" 2>/dev/null || true
    sleep 1
    kill -9 "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    echo ""
    echo "Server output:"
    cat /tmp/server_output.txt | grep -E "Statistics|messages|Processor" || true
    print_success "Client stress test complete"
}
mode_test_client_multi_symbol() {
    require_client_built
    local scenario="${1:-30}"
    local port="${2:-$DEFAULT_PORT}"
    print_status "Client Multi-Symbol Test (Dual-Processor)"
    echo ""
    "./${BUILD_DIR}/matching_engine" --tcp "$port" --dual-processor > /tmp/server_output.txt 2>&1 &
    local server_pid=$!
    sleep 1
    if ! kill -0 "$server_pid" 2>/dev/null; then
        print_error "Server failed to start"
        exit 1
    fi
    print_status "Server started in dual-processor mode (PID: $server_pid)"
    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" localhost "$port"
    kill -TERM "$server_pid" 2>/dev/null || true
    sleep 1
    kill -9 "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    echo ""
    echo "Server processor distribution:"
    cat /tmp/server_output.txt | grep -E "Processor|messages" || true
    print_success "Client multi-symbol test complete"
}
mode_test_client_all() {
    require_client_built
    local port="${1:-$DEFAULT_PORT}"
    print_status "Running All Client Tests"
    echo ""
    echo "=========================================="
    echo "Client Test Suite"
    echo "=========================================="
    echo ""
    "./${BUILD_DIR}/matching_engine" --tcp "$port" --dual-processor > /tmp/server_output.txt 2>&1 &
    local server_pid=$!
    sleep 1
    if ! kill -0 "$server_pid" 2>/dev/null; then
        print_error "Server failed to start"
        exit 1
    fi
    print_status "Server started (PID: $server_pid)"
    echo ""
    echo "=== Basic Scenarios ==="
    "./${BUILD_DIR}/matching_engine_client" --scenario 1 --quiet localhost "$port"
    "./${BUILD_DIR}/matching_engine_client" --scenario 2 --quiet localhost "$port"
    "./${BUILD_DIR}/matching_engine_client" --scenario 3 --quiet localhost "$port"
    print_success "Basic scenarios passed"
    echo ""
    echo "=== Stress Test (1K orders) ==="
    "./${BUILD_DIR}/matching_engine_client" --scenario 10 localhost "$port"
    echo ""
    echo "=== Multi-Symbol Test (10K orders) ==="
    "./${BUILD_DIR}/matching_engine_client" --scenario 30 localhost "$port"
    echo ""
    echo "=== Matching Test (1K pairs) ==="
    "./${BUILD_DIR}/matching_engine_client" --scenario 20 localhost "$port"
    echo ""
    kill -TERM "$server_pid" 2>/dev/null || true
    sleep 1
    kill -9 "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
    print_success "All client tests complete!"
}
list_client_scenarios() {
    require_client_built
    "./${BUILD_DIR}/matching_engine_client" --list-scenarios
}
# -------------------------
# Other test modes
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
    print_status "Running valgrind on unit tests (AVX-512-free build)..."
    echo ""
    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
        --error-exitcode=1 \
        "./${VALGRIND_BUILD_DIR}/matching_engine_tests"
    
    print_success "Valgrind memory check passed - no leaks detected!"
}
run_valgrind_server_udp() {
    if [ "$PLATFORM" != "Linux" ]; then
        print_error "valgrind only supported on Linux"
        exit 1
    fi
    if ! command_exists valgrind; then
        print_error "valgrind not found. Install with: sudo apt install valgrind"
        exit 1
    fi
    require_valgrind_built
    local timeout_secs="${1:-5}"
    print_status "Running valgrind on UDP server for ${timeout_secs} seconds..."
    echo ""
    timeout --signal=SIGINT "${timeout_secs}" \
        valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
        "./${VALGRIND_BUILD_DIR}/matching_engine" --udp "$DEFAULT_PORT" || true
    print_success "Valgrind UDP server check complete"
}
run_valgrind_server() {
    if [ "$PLATFORM" != "Linux" ]; then
        print_error "valgrind only supported on Linux"
        exit 1
    fi
    if ! command_exists valgrind; then
        print_error "valgrind not found. Install with: sudo apt install valgrind"
        exit 1
    fi
    require_valgrind_built
    local timeout_secs="${1:-5}"
    
    print_status "Running valgrind on TCP server for ${timeout_secs} seconds..."
    echo ""
    
    timeout --signal=SIGINT "${timeout_secs}" \
        valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
        "./${VALGRIND_BUILD_DIR}/matching_engine" --tcp "$DEFAULT_PORT" || true
    
    print_success "Valgrind server check complete"
}
run_server() {
    if [ ! -x "$BUILD_DIR/matching_engine" ]; then
        print_error "matching_engine not built. Run ./build.sh build first."
        exit 1
    fi
    shift
    print_status "Starting matching engine..."
    "./${BUILD_DIR}/matching_engine" "$@"
}
show_info() {
    cmake --build "$BUILD_DIR" --target info
}
show_help() {
    cat << EOF
Usage:
  ./build.sh [command] [args...]

Build:
  build            Build Release
  debug            Build Debug
  release          Build Release
  rebuild          Clean + rebuild
  configure        Configure only
  clean            Remove build dir
  clean-all        Remove all build dirs (including valgrind)
  valgrind-build   Build valgrind-compatible version (no AVX-512)

Server Tests:
  test             Run Unity unit tests (C tests)

README Run-Modes (2-terminal workflows):
  test-binary          UDP server CSV output + binary_client scenarios
  test-binary-full     UDP server binary output piped to decoder
  test-tcp             TCP server CSV output + binary_client --tcp
  test-tcp-csv         TCP server CSV output + binary_client --tcp --csv
  test-dual-processor  TCP server dual-processor mode (symbol routing)
  test-single-processor TCP server single-processor mode
  test-multicast       TCP server with multicast market data feed

Client Tests (auto-starts server):
  test-client          Run basic client scenarios (1-3)
  test-client-stress [N]  Run stress test (default: scenario 11)
  test-client-multi    Run multi-symbol test (dual-processor)
  test-client-all      Run full client test suite

Client Commands:
  client [host] [port]     Interactive client mode
  client-scenario N [host] [port]  Run specific scenario (auto-detect transport/encoding)
  client-udp N [host] [port]       Run scenario via UDP (CSV encoding)
  client-udp-binary N [host] [port] Run scenario via UDP (binary encoding)
  client-tcp N [host] [port]       Run scenario via TCP (CSV encoding)
  client-tcp-binary N [host] [port] Run scenario via TCP (binary encoding)
  client-stress [N] [host] [port]  Run stress scenario (default: 11)
  client-burst [N] [host] [port]   Run burst scenario (DANGER! default: 40)
  client-multicast [host] [port]   Client with multicast subscription
  client-mcast-only [group] [port] Multicast-only subscriber (no orders)
  client-scenarios         List available scenarios

Memory Checking:
  valgrind             Run unit tests under valgrind
  valgrind-server [s]  Run TCP server under valgrind for [s] seconds
  valgrind-server-udp  Run UDP server under valgrind

Run Server:
  run [args]           Run server directly
  run-tcp              Run TCP server on ${DEFAULT_PORT}
  run-udp              Run UDP server on ${DEFAULT_PORT}
  run-dual             Run TCP server in dual-processor mode
  run-single           Run TCP server in single-processor mode
  run-multicast        Run TCP server with multicast feed
  run-multicast-binary Run TCP with binary multicast
  run-udp-binary       Run UDP with binary output (piped through decoder)
  run-udp-binary-raw   Run UDP with raw binary output (no decoder)
  run-tcp-binary       Run TCP with binary output

Examples:
  ./build.sh build
  ./build.sh test-client
  ./build.sh test-client-stress 12      # 100K order stress test
  ./build.sh client-scenario 2          # Run scenario 2 (auto-detect)
  ./build.sh client-udp 1               # Run scenario 1 via UDP
  ./build.sh client-udp-binary 1        # Run scenario 1 via UDP with binary
  ./build.sh client-tcp-binary 2        # Run scenario 2 via TCP with binary
  ./build.sh client-burst 41            # 1M orders, no throttling (DANGER!)
  
  # Two terminal workflow (UDP binary):
  # Terminal 1: ./build.sh run-udp-binary
  # Terminal 2: ./build.sh client-udp-binary 1
  
  # Two terminal workflow (TCP binary):
  # Terminal 1: ./build.sh run-tcp-binary
  # Terminal 2: ./build.sh client-tcp-binary 1
  
  # Two terminal workflow (multicast):
  # Terminal 1: ./build.sh run-multicast
  # Terminal 2: ./build.sh client-multicast
EOF
}
# Normalize --flags → commands
case "$1" in
    --clean) set "clean" ;;
    --help) set "help" ;;
    --build) set "build" ;;
    --debug) set "debug" ;;
    --release) set "release" ;;
    --test) set "test" ;;
esac
main() {
    if [ $# -eq 0 ]; then
        show_help
        exit 0
    fi
    GENERATOR=$(detect_generator)
    case "$1" in
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
        release)
            BUILD_TYPE="Release"
            clean
            configure "$BUILD_TYPE" "$GENERATOR"
            build
            ;;
        rebuild)
            clean
            configure "$BUILD_TYPE" "$GENERATOR"
            build
            ;;
        configure)
            configure "$BUILD_TYPE" "$GENERATOR"
            ;;
        clean)
            clean
            ;;
        clean-all)
            clean
            clean_valgrind
            ;;
        valgrind-build)
            build_for_valgrind
            ;;
        test)
            run_unit_tests
            ;;
        test-binary)
            shift
            mode_test_binary "$@"
            ;;
        test-binary-full)
            shift
            mode_test_binary_full "$@"
            ;;
        test-tcp)
            shift
            mode_test_tcp "$@"
            ;;
        test-tcp-csv)
            shift
            mode_test_tcp_csv "$@"
            ;;
        test-dual-processor|test-dual)
            shift
            mode_test_dual_processor "$@"
            ;;
        test-single-processor|test-single)
            shift
            mode_test_single_processor "$@"
            ;;
        test-multicast)
            shift
            mode_test_multicast "$@"
            ;;
        test-all)
            print_status "README run-modes quick reference:"
            echo ""
            echo "1) UDP + Binary (decoded):"
            echo "   Terminal 1: ./${BUILD_DIR}/matching_engine --udp --binary ${DEFAULT_PORT} 2>/dev/null | ./${BUILD_DIR}/binary_decoder"
            echo "   Terminal 2: ./${BUILD_DIR}/binary_client ${DEFAULT_PORT} 1"
            echo ""
            echo "2) TCP + Binary scenario then interactive:"
            echo "   Terminal 1: ./${BUILD_DIR}/matching_engine --tcp ${DEFAULT_PORT}"
            echo "   Terminal 2: ./${BUILD_DIR}/binary_client ${DEFAULT_PORT} 2 --tcp"
            echo ""
            echo "3) Multicast Market Data:"
            echo "   Terminal 1: ./${BUILD_DIR}/matching_engine --tcp ${DEFAULT_PORT} --multicast ${MULTICAST_GROUP}:${MULTICAST_PORT}"
            echo "   Terminal 2: ./${BUILD_DIR}/multicast_subscriber ${MULTICAST_GROUP} ${MULTICAST_PORT}"
            echo "   Terminal 3: ./${BUILD_DIR}/matching_engine_client localhost ${DEFAULT_PORT}"
            echo ""
            ;;
        # Client commands
        client|run-client)
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
        client-udp-binary)
            shift
            mode_client_udp_binary "$@"
            ;;
        client-tcp)
            shift
            mode_client_tcp "$@"
            ;;
        client-tcp-binary)
            shift
            mode_client_tcp_binary "$@"
            ;;
        client-stress)
            shift
            mode_client_stress "$@"
            ;;
        client-burst)
            shift
            mode_client_burst "$@"
            ;;
        client-multicast)
            shift
            mode_client_multicast "$@"
            ;;
        client-mcast-only)
            shift
            mode_client_mcast_only "$@"
            ;;
        
        # Client tests
        test-client|test-client-basic)
            shift
            mode_test_client_basic "$@"
            ;;
        test-client-stress)
            shift
            mode_test_client_stress "$@"
            ;;
        test-client-multi|test-client-multi-symbol)
            shift
            mode_test_client_multi_symbol "$@"
            ;;
        test-client-all)
            shift
            mode_test_client_all "$@"
            ;;
        client-scenarios|list-scenarios)
            list_client_scenarios
            ;;
        # Valgrind
        valgrind)
            run_valgrind
            ;;
        valgrind-server)
            shift
            run_valgrind_server "$@"
            ;;
        valgrind-server-udp)
            shift
            run_valgrind_server_udp "$@"
            ;;
        # Run server
        run)
            run_server "$@"
            ;;
        run-tcp)
            run_server run --tcp "$DEFAULT_PORT"
            ;;
        run-udp)
            run_server run --udp "$DEFAULT_PORT"
            ;;
        run-dual)
            run_server run --tcp "$DEFAULT_PORT" --dual-processor
            ;;
        run-single)
            run_server run --tcp "$DEFAULT_PORT" --single-processor
            ;;
        run-multicast)
            run_server run --tcp "$DEFAULT_PORT" --multicast "${MULTICAST_GROUP}:${MULTICAST_PORT}"
            ;;
        run-multicast-binary)
            run_server run --tcp "$DEFAULT_PORT" --binary --multicast "${MULTICAST_GROUP}:${MULTICAST_PORT}"
            ;;
        run-udp-binary)
            require_built
            require_decoder_built
            print_status "Starting matching engine (UDP binary -> decoder)..."
            "./${BUILD_DIR}/matching_engine" --udp "$DEFAULT_PORT" --binary | "./${BUILD_DIR}/binary_decoder"
            ;;
        run-udp-binary-raw)
            run_server run --udp "$DEFAULT_PORT" --binary
            ;;
        run-tcp-binary)
            run_server run --tcp "$DEFAULT_PORT" --binary
            ;;
        # Info
        info)
            show_info
            ;;
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
