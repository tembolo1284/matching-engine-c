#!/bin/bash
# build.sh - Build script for Matching Engine
set -e
BUILD_DIR="build"
BUILD_DIR_DPDK="build-dpdk"
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
# ============================================================================
# DPDK Detection
# ============================================================================
check_dpdk_available() {
    if pkg-config --exists libdpdk 2>/dev/null; then
        return 0
    else
        return 1
    fi
}
check_dpdk_ready() {
    # Check if DPDK is installed
    if ! check_dpdk_available; then
        print_error "DPDK not installed"
        echo "  Install: sudo apt install dpdk dpdk-dev libdpdk-dev"
        return 1
    fi
    
    # Check huge pages
    local huge_total=$(grep HugePages_Total /proc/meminfo 2>/dev/null | awk '{print $2}')
    if [ -z "$huge_total" ] || [ "$huge_total" -eq 0 ]; then
        print_error "Huge pages not allocated"
        echo "  Run: sudo sh -c 'echo 1024 > /proc/sys/vm/nr_hugepages'"
        return 1
    fi
    
    # Check UIO module
    if ! lsmod | grep -q uio; then
        print_warning "UIO module not loaded"
        echo "  Run: sudo modprobe uio_pci_generic"
    fi
    
    return 0
}
# ============================================================================
# Configuration and Build
# ============================================================================
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
        rm -rf "$BUILD_DIR_DPDK"
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
clean_dpdk() {
    if [ -d "$BUILD_DIR_DPDK" ]; then
        print_status "Cleaning DPDK build directory..."
        rm -rf "$BUILD_DIR_DPDK"
        print_success "Clean complete"
    else
        print_warning "DPDK build directory does not exist"
    fi
}
# ============================================================================
# Requirements Checks
# ============================================================================
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
require_multicast_subscriber_built() {
    if [ ! -x "$BUILD_DIR/multicast_subscriber" ]; then
        print_error "multicast_subscriber not built. Run ./build.sh build first."
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
require_dpdk_built() {
    if [ ! -x "$BUILD_DIR_DPDK/matching_engine" ]; then
        print_error "DPDK build not found."
        print_status "Building DPDK version..."
        build_dpdk
    fi
}
# ============================================================================
# Specialized Builds
# ============================================================================
build_for_valgrind() {
    print_status "Building valgrind-compatible version (no AVX-512)..."
    clean_valgrind
    configure "Debug" "$GENERATOR" "$VALGRIND_BUILD_DIR" "-DVALGRIND_BUILD=ON"
    build "$VALGRIND_BUILD_DIR"
    print_success "Valgrind build complete"
}
build_dpdk() {
    local vdev="${1:-null}"
    
    print_status "Building with DPDK kernel bypass..."
    
    if [ "$PLATFORM" != "Linux" ]; then
        print_error "DPDK is only supported on Linux"
        exit 1
    fi
    
    if ! check_dpdk_available; then
        print_error "DPDK not installed"
        echo "  Install: sudo apt install dpdk dpdk-dev libdpdk-dev"
        exit 1
    fi
    
    # Show DPDK version
    local dpdk_version=$(pkg-config --modversion libdpdk 2>/dev/null)
    echo "  DPDK version: $dpdk_version"
    echo "  Virtual device: $vdev"
    
    clean_dpdk
    configure "Release" "$GENERATOR" "$BUILD_DIR_DPDK" "-DUSE_DPDK=ON -DDPDK_VDEV=$vdev"
    build "$BUILD_DIR_DPDK"
    print_success "DPDK build complete"
    
    echo ""
    echo "=========================================="
    echo "DPDK Build Complete"
    echo "=========================================="
    echo "  Binary: $BUILD_DIR_DPDK/matching_engine"
    echo "  Run:    sudo ./$BUILD_DIR_DPDK/matching_engine"
    echo ""
    echo "Note: DPDK requires sudo for huge pages access"
    echo "=========================================="
}
build_multicast_subscriber() {
    print_status "Building multicast_subscriber..."
    if [ ! -f "$BUILD_DIR/CMakeCache.txt" ]; then
        configure "$BUILD_TYPE" "$GENERATOR" "$BUILD_DIR"
    fi
    build "$BUILD_DIR" multicast_subscriber
    print_success "multicast_subscriber build complete"
}
# ============================================================================
# Server Run Modes (Standard Sockets)
# ============================================================================
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
    echo "  Backend:   Standard Sockets"
    echo "=========================================="
    echo ""
    "./${BUILD_DIR}/matching_engine" "$@"
}
run_quiet() {
    require_built
    print_status "Starting Unified Server (Binary + Quiet Mode)"
    "./${BUILD_DIR}/matching_engine" --binary --quiet "$@"
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
# ============================================================================
# Server Run Modes (DPDK Kernel Bypass)
# ============================================================================
run_dpdk() {
    require_dpdk_built
    
    print_status "Starting DPDK Server (Kernel Bypass)"
    echo ""
    echo "=========================================="
    echo "  Matching Engine - DPDK Kernel Bypass"
    echo "=========================================="
    echo "  TCP:       port $TCP_PORT (kernel)"
    echo "  UDP:       port $UDP_PORT (DPDK)"
    echo "  Multicast: $MULTICAST_GROUP:$MULTICAST_PORT (DPDK)"
    echo "  Backend:   DPDK (kernel bypass for UDP)"
    echo ""
    echo "  Note: Requires sudo for huge pages"
    echo "=========================================="
    echo ""
    sudo "./${BUILD_DIR_DPDK}/matching_engine" "$@"
}
run_dpdk_quiet() {
    require_dpdk_built
    print_status "Starting DPDK Server (Quiet/Benchmark Mode)"
    echo ""
    sudo "./${BUILD_DIR_DPDK}/matching_engine" --quiet "$@"
}
run_dpdk_benchmark() {
    require_dpdk_built
    print_status "Starting DPDK Server (Binary + Quiet)"
    echo ""
    sudo "./${BUILD_DIR_DPDK}/matching_engine" --binary --quiet "$@"
}
test_dpdk() {
    if [ ! -x "$BUILD_DIR_DPDK/dpdk_test" ]; then
        print_error "DPDK test not built. Run ./build.sh build-dpdk first."
        exit 1
    fi
    
    print_status "Running DPDK Transport Tests"
    echo ""
    echo "Note: Requires sudo for huge pages access"
    echo ""
    sudo "./${BUILD_DIR_DPDK}/dpdk_test"
}
check_dpdk() {
    echo "=========================================="
    echo "  DPDK Prerequisites Check"
    echo "=========================================="
    echo ""
    
    # Check installation
    echo "DPDK Installation:"
    if check_dpdk_available; then
        local ver=$(pkg-config --modversion libdpdk 2>/dev/null)
        echo "  ✅ libdpdk: $ver"
    else
        echo "  ❌ libdpdk: NOT FOUND"
        echo "     Install: sudo apt install dpdk dpdk-dev libdpdk-dev"
    fi
    
    if command_exists dpdk-devbind.py; then
        echo "  ✅ dpdk-devbind.py: $(which dpdk-devbind.py)"
    else
        echo "  ❌ dpdk-devbind.py: NOT FOUND"
    fi
    echo ""
    
    # Check huge pages
    echo "Huge Pages:"
    local huge_total=$(grep HugePages_Total /proc/meminfo 2>/dev/null | awk '{print $2}')
    local huge_free=$(grep HugePages_Free /proc/meminfo 2>/dev/null | awk '{print $2}')
    local huge_size=$(grep Hugepagesize /proc/meminfo 2>/dev/null | awk '{print $2}')
    
    if [ -n "$huge_total" ] && [ "$huge_total" -gt 0 ]; then
        echo "  ✅ Allocated: $huge_total pages (${huge_size}KB each)"
        echo "     Free: $huge_free pages"
        echo "     Total: $(( huge_total * huge_size / 1024 )) MB"
    else
        echo "  ❌ Not allocated"
        echo "     Run: sudo sh -c 'echo 1024 > /proc/sys/vm/nr_hugepages'"
    fi
    
    if mount | grep -q hugetlbfs; then
        echo "  ✅ hugetlbfs mounted"
    else
        echo "  ❌ hugetlbfs not mounted"
    fi
    echo ""
    
    # Check kernel modules
    echo "Kernel Modules:"
    if lsmod | grep -q uio_pci_generic; then
        echo "  ✅ uio_pci_generic: loaded"
    else
        echo "  ❌ uio_pci_generic: not loaded"
        echo "     Run: sudo modprobe uio_pci_generic"
    fi
    echo ""
    
    # Check NICs
    echo "Network Devices:"
    if command_exists dpdk-devbind.py; then
        dpdk-devbind.py --status-dev net 2>/dev/null | head -20
    else
        lspci | grep -i ethernet
    fi
    echo ""
    
    # Summary
    echo "=========================================="
    if check_dpdk_available; then
        local huge_ok=false
        [ -n "$huge_total" ] && [ "$huge_total" -gt 0 ] && huge_ok=true
        
        if $huge_ok; then
            echo "✅ System ready for DPDK!"
            echo "   Build: ./build.sh build-dpdk"
            echo "   Run:   ./build.sh run-dpdk"
        else
            echo "⚠️  DPDK installed but huge pages not configured"
        fi
    else
        echo "❌ DPDK not installed"
    fi
    echo "=========================================="
}
# ============================================================================
# Multicast Subscriber Tool
# ============================================================================
run_multicast_subscriber() {
    require_multicast_subscriber_built
    local group="$MULTICAST_GROUP"
    local port="$MULTICAST_PORT"
    if [ $# -ge 1 ]; then
        if [[ "$1" == *:* ]]; then
            group="${1%%:*}"
            port="${1##*:}"
        else
            group="$1"
            if [ $# -ge 2 ]; then
                port="$2"
            fi
        fi
    fi
    print_status "Starting Multicast Subscriber"
    echo ""
    echo "=========================================="
    echo "  Multicast Subscriber"
    echo "=========================================="
    echo "  Group: $group"
    echo "  Port:  $port"
    echo "=========================================="
    echo ""
    "./${BUILD_DIR}/multicast_subscriber" "$group" "$port"
}
# ============================================================================
# Benchmark Modes
# ============================================================================
benchmark_matching() {
    require_client_built
    local scenario="${1:-24}"
    print_status "Matching Benchmark Test (Scenario ${scenario})"
    echo ""
    echo "=========================================="
    echo "Matching Benchmark - Sustained Throughput"
    echo "=========================================="
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
benchmark_dpdk() {
    require_dpdk_built
    require_client_built
    local scenario="${1:-24}"
    
    print_status "DPDK Benchmark Test (Scenario ${scenario})"
    echo ""
    echo "=========================================="
    echo "  DPDK Kernel Bypass Benchmark"
    echo "=========================================="
    echo ""
    
    sudo "./${BUILD_DIR_DPDK}/matching_engine" --quiet 2>&1 &
    local server_pid=$!
    sleep 2
    
    if ! sudo kill -0 "$server_pid" 2>/dev/null; then
        print_error "DPDK server failed to start"
        exit 1
    fi
    print_status "DPDK Server started (PID: $server_pid)"
    echo ""
    
    "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --udp localhost "$UDP_PORT"
    sleep 2
    
    print_status "Shutting down server..."
    sudo kill -TERM "$server_pid" 2>/dev/null || true
    sudo wait "$server_pid" 2>/dev/null || true
    print_success "DPDK benchmark complete"
}
# ============================================================================
# Client Run Modes
# ============================================================================
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
# New: quiet client scenario for benchmarks (no debug prints)
client_scenario_quiet() {
    require_client_built
    local scenario="${1:-1}"
    local transport="${2:-tcp}"
    local host="localhost"
    local port="$TCP_PORT"
    if [ "$transport" = "udp" ]; then
        port="$UDP_PORT"
    fi
    print_status "Client Scenario Mode - QUIET (scenario $scenario, $transport)"
    if [ "$transport" = "udp" ]; then
        "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --quiet --udp "$host" "$port"
    else
        "./${BUILD_DIR}/matching_engine_client" --scenario "$scenario" --quiet "$host" "$port"
    fi
}
list_scenarios() {
    require_client_built
    "./${BUILD_DIR}/matching_engine_client" --list-scenarios
}
# ============================================================================
# Test Modes
# ============================================================================
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
# ============================================================================
# Help
# ============================================================================
show_help() {
    cat << EOF
Matching Engine - Build Script
==============================
Usage: ./build.sh [command] [args...]
BUILD COMMANDS
  build                    Build Release (socket mode)
  build-dpdk [vdev]        Build with DPDK kernel bypass
                           vdev: null (default), ring, or empty for physical NIC
  debug                    Build Debug
  rebuild                  Clean + rebuild
  clean                    Remove all build directories
  multicast-sub-build      Build only multicast_subscriber tool
TEST COMMANDS
  test                     Run unit tests
  test-dpdk                Run DPDK transport tests (requires sudo)
  valgrind                 Run unit tests under valgrind
  valgrind-server          Run server under valgrind
DPDK COMMANDS
  check-dpdk               Check DPDK prerequisites
  run-dpdk                 Start DPDK server (requires sudo)
  run-dpdk-quiet           Start DPDK server (benchmark mode)
  run-dpdk-benchmark       Start DPDK server (binary + quiet)
  benchmark-dpdk N         DPDK benchmark (requires sudo)
SERVER COMMANDS (Standard Sockets)
  run                      Start server (CSV, dual processor)
  run-binary               Start server (binary format)
  run-quiet                Start server (binary + quiet, no debug prints)
  run-benchmark            Start server (binary + quiet)
  Server always listens on:
    TCP:       $TCP_PORT
    UDP:       $UDP_PORT
    Multicast: $MULTICAST_GROUP:$MULTICAST_PORT
TOOLS
  multicast-sub [GROUP [PORT]]   Run multicast subscriber
CLIENT COMMANDS
  client [tcp|udp]               Interactive client
  client-scenario N [tcp|udp]    Run scenario N (with debug prints)
  client-scenario-quiet N [tcp|udp]  Run scenario N (no debug prints)
  scenarios                      List available scenarios
BENCHMARK COMMANDS
  benchmark-match N              Matching benchmark (socket)
  benchmark-dual N               Dual-processor benchmark (socket)
  benchmark-dpdk N               Matching benchmark (DPDK)
DPDK SETUP (Linux only)
  1. Install: sudo apt install dpdk dpdk-dev libdpdk-dev
  2. Huge pages: sudo sh -c 'echo 1024 > /proc/sys/vm/nr_hugepages'
  3. Module: sudo modprobe uio_pci_generic
  4. Check: ./build.sh check-dpdk
  5. Build: ./build.sh build-dpdk
  6. Run: ./build.sh run-dpdk
EXAMPLES
  ./build.sh build                       # Build with sockets (default)
  ./build.sh build-dpdk                  # Build with DPDK (net_null vdev)
  ./build.sh build-dpdk ring             # Build with DPDK (net_ring vdev)
  ./build.sh check-dpdk                  # Check DPDK prerequisites
  ./build.sh run                         # Start server (sockets)
  ./build.sh run-quiet                   # Start server (binary + quiet)
  ./build.sh run-dpdk                    # Start server (DPDK, requires sudo)
  ./build.sh client-scenario-quiet 22    # Run 100K benchmark cleanly
  ./build.sh benchmark-dpdk 24           # DPDK benchmark (10M pairs)
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
        build-dpdk)
            shift
            build_dpdk "$@"
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
            clean_dpdk
            ;;
        multicast-sub-build)
            shift
            build_multicast_subscriber "$@"
            ;;
        # DPDK
        check-dpdk)
            check_dpdk
            ;;
        run-dpdk)
            shift
            run_dpdk "$@"
            ;;
        run-dpdk-quiet)
            shift
            run_dpdk_quiet "$@"
            ;;
        run-dpdk-benchmark)
            shift
            run_dpdk_benchmark "$@"
            ;;
        test-dpdk)
            test_dpdk
            ;;
        benchmark-dpdk)
            shift
            benchmark_dpdk "$@"
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
        # Server (Sockets)
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
        client-scenario-quiet)
            shift
            client_scenario_quiet "$@"
            ;;
        scenarios|list-scenarios)
            list_scenarios
            ;;
        # Tools
        multicast-sub|multicast-subscriber|mcast-sub)
            shift
            run_multicast_subscriber "$@"
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
