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
# README run-modes
# -------------------------

mode_test_binary() {
    # README "UDP + CSV output, binary client scenarios (default behavior)"
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
    echo "Notes:"
    echo "  - Server outputs CSV (human readable)."
    echo "  - Each binary_client scenario is fire-and-forget."
    echo ""

    print_status "Starting UDP server now (CSV output). Ctrl+C to stop."
    "./${BUILD_DIR}/matching_engine" --udp "${port}"
}

mode_test_binary_full() {
    # README "UDP + binary output piped to decoder"
    require_built
    if [ ! -x "$BUILD_DIR/binary_decoder" ]; then
        print_error "binary_decoder not built. Run ./build.sh build first."
        exit 1
    fi

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
    echo "Notes:"
    echo "  - Server outputs binary; decoder makes it readable."
    echo "  - Each scenario is fire-and-forget."
    echo ""

    print_status "Starting UDP server now (binary output -> decoder). Ctrl+C to stop."
    "./${BUILD_DIR}/matching_engine" --udp --binary "${port}" 2>/dev/null | "./${BUILD_DIR}/binary_decoder"
}

mode_test_tcp() {
    # README "TCP + Binary protocol (scenario then interactive)"
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
    echo "Notes:"
    echo "  - After a scenario, the TCP client enters interactive mode."
    echo "  - Type 'quit' in the client to exit."
    echo ""

    print_status "Starting TCP server now (CSV output). Ctrl+C to stop."
    "./${BUILD_DIR}/matching_engine" --tcp "${port}"
}

mode_test_tcp_csv() {
    # README "TCP + CSV protocol (scenario then interactive)"
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
    echo "Notes:"
    echo "  - Client sends CSV messages over TCP."
    echo "  - Client enters interactive mode after scenario."
    echo ""

    print_status "Starting TCP server now (CSV output). Ctrl+C to stop."
    "./${BUILD_DIR}/matching_engine" --tcp "${port}"
}

mode_test_dual_processor() {
    # Test dual-processor symbol routing
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
    echo "  > buy AAPL 150 30 3     # → Processor 0 (A is A-M)"
    echo "  > buy TSLA 180 40 4     # → Processor 1 (T is N-Z)"
    echo "  > flush"
    echo "  > quit"
    echo ""
    echo "After Ctrl+C, check per-processor statistics:"
    echo "  - Messages to Processor 0: Should show IBM, AAPL orders"
    echo "  - Messages to Processor 1: Should show NVDA, TSLA orders"
    echo ""

    print_status "Starting TCP server in DUAL-PROCESSOR mode. Ctrl+C to stop and see stats."
    "./${BUILD_DIR}/matching_engine" --tcp "${port}" --dual-processor
}

mode_test_single_processor() {
    # Test single-processor mode for comparison
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
    echo "Terminal 1 (this terminal):"
    echo "  Server starts in single-processor mode"
    echo ""
    echo "Terminal 2 (run these commands):"
    echo "  ./${BUILD_DIR}/tcp_client localhost ${port}"
    echo "  > buy IBM 100 50 1"
    echo "  > buy NVDA 200 25 2"
    echo "  > flush"
    echo "  > quit"
    echo ""

    print_status "Starting TCP server in SINGLE-PROCESSOR mode. Ctrl+C to stop."
    "./${BUILD_DIR}/matching_engine" --tcp "${port}" --single-processor
}

mode_test_multicast() {
    # Test multicast market data feed
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
    echo "Real-world market data distribution pattern!"
    echo "  - Server sends ONCE to multicast group"
    echo "  - Network delivers to ALL subscribers"
    echo "  - Unlimited subscribers, zero server overhead"
    echo ""
    echo "Terminal 1 (this terminal):"
    echo "  Server with multicast broadcasting"
    echo ""
    echo "Terminal 2 (subscriber - can run multiple instances):"
    echo "  ./${BUILD_DIR}/multicast_subscriber ${mcast_group} ${mcast_port}"
    echo ""
    echo "Terminal 3 (send orders via TCP):"
    echo "  ./${BUILD_DIR}/tcp_client localhost ${port}"
    echo "  > buy IBM 100 50 1"
    echo "  > sell IBM 100 30 2"
    echo "  > flush"
    echo ""
    echo "Notes:"
    echo "  - ALL subscribers receive market data simultaneously"
    echo "  - Start multiple subscribers to see multicast in action"
    echo "  - Works across machines if network supports multicast"
    echo ""
    echo "=========================================="
    echo ""

    print_status "Starting TCP server with MULTICAST feed. Ctrl+C to stop."
    "./${BUILD_DIR}/matching_engine" --tcp "${port}" --multicast "${mcast_group}:${mcast_port}"
}

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

    # Ensure valgrind-compatible build exists
    require_valgrind_built

    print_status "Running valgrind on unit tests (AVX-512-free build)..."
    echo ""
    echo "Note: Using $VALGRIND_BUILD_DIR (built without AVX-512 for Valgrind compatibility)"
    echo ""
    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
        --error-exitcode=1 \
        "./${VALGRIND_BUILD_DIR}/matching_engine_tests"
    
    print_success "Valgrind memory check passed - no leaks detected!"
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

    # Ensure valgrind-compatible build exists
    require_valgrind_built

    local timeout_secs="${1:-5}"
    
    print_status "Running valgrind on TCP server for ${timeout_secs} seconds..."
    echo ""
    echo "Note: Using $VALGRIND_BUILD_DIR (built without AVX-512 for Valgrind compatibility)"
    echo "      Server will auto-terminate after ${timeout_secs} seconds."
    echo ""
    
    # Use timeout to auto-terminate the server
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
  ./build.sh [command] [port] [scenarios...]

Build:
  build            Build Release
  debug            Build Debug
  release          Build Release
  rebuild          Clean + rebuild
  configure        Configure only
  clean            Remove build dir
  clean-all        Remove all build dirs (including valgrind)
  valgrind-build   Build valgrind-compatible version (no AVX-512)

Real Tests:
  test             Run Unity unit tests (C tests)

README Run-Modes (2-terminal workflows):
  test-binary          UDP server CSV output + binary_client scenarios
  test-binary-full     UDP server binary output piped to decoder
  test-tcp             TCP server CSV output + binary_client --tcp
  test-tcp-csv         TCP server CSV output + binary_client --tcp --csv
  test-dual-processor  TCP server dual-processor mode (symbol routing test)
  test-single-processor TCP server single-processor mode (comparison)
  test-multicast       TCP server with multicast market data feed (NEW!)
  test-all             Prints all README run-modes (does not start anything)

Memory Checking:
  valgrind             Run unit tests under valgrind (auto-exits)
  valgrind-server [s]  Run TCP server under valgrind for [s] seconds (default: 5)

Scenario examples:
  ./build.sh test-binary 1234 all
  ./build.sh test-binary-full 1234 1 2 3
  ./build.sh test-tcp 1234 2
  ./build.sh test-tcp-csv 1234 2
  ./build.sh test-dual-processor 1234
  ./build.sh test-multicast 1234 239.255.0.1 5000

Run:
  run [args]       Run server directly (same as README)
  run-tcp          Run TCP server on 1234 (dual-processor, default)
  run-udp          Run UDP server on 1234
  run-dual         Run TCP server in dual-processor mode (default)
  run-single       Run TCP server in single-processor mode
  run-multicast    Run TCP server with multicast feed (239.255.0.1:5000)
  run-multicast-binary Run TCP server with binary multicast feed

Multicast Info:
  Multicast provides zero-overhead broadcast to unlimited subscribers.
  Perfect for market data distribution (CME, NASDAQ, ICE use this).
  Default group: ${MULTICAST_GROUP}:${MULTICAST_PORT} (local subnet)

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
            # Print recipes only; don't start servers
            print_status "README run-modes quick reference:"
            echo ""
            echo "1) UDP + Binary (decoded):"
            echo "   Terminal 1: ./${BUILD_DIR}/matching_engine --udp --binary ${DEFAULT_PORT} 2>/dev/null | ./${BUILD_DIR}/binary_decoder"
            echo "   Terminal 2: ./${BUILD_DIR}/binary_client ${DEFAULT_PORT} 1"
            echo "               ./${BUILD_DIR}/binary_client ${DEFAULT_PORT} 2"
            echo "               ./${BUILD_DIR}/binary_client ${DEFAULT_PORT} 3"
            echo ""
            echo "2) UDP + CSV:"
            echo "   Terminal 1: ./${BUILD_DIR}/matching_engine --udp ${DEFAULT_PORT}"
            echo "   Terminal 2: ./${BUILD_DIR}/binary_client ${DEFAULT_PORT} 1 --csv"
            echo "               ./${BUILD_DIR}/binary_client ${DEFAULT_PORT} 2 --csv"
            echo "               ./${BUILD_DIR}/binary_client ${DEFAULT_PORT} 3 --csv"
            echo ""
            echo "3) TCP + Binary scenario then interactive:"
            echo "   Terminal 1: ./${BUILD_DIR}/matching_engine --tcp ${DEFAULT_PORT}"
            echo "   Terminal 2: ./${BUILD_DIR}/binary_client ${DEFAULT_PORT} 2 --tcp"
            echo ""
            echo "4) TCP + CSV scenario then interactive:"
            echo "   Terminal 1: ./${BUILD_DIR}/matching_engine --tcp ${DEFAULT_PORT}"
            echo "   Terminal 2: ./${BUILD_DIR}/binary_client ${DEFAULT_PORT} 2 --tcp --csv"
            echo ""
            echo "5) Dual-Processor Mode (default):"
            echo "   Terminal 1: ./${BUILD_DIR}/matching_engine --tcp ${DEFAULT_PORT}"
            echo "   Terminal 2: ./${BUILD_DIR}/tcp_client localhost ${DEFAULT_PORT}"
            echo "               > buy IBM 100 50 1      # → Processor 0 (A-M)"
            echo "               > buy NVDA 200 25 2     # → Processor 1 (N-Z)"
            echo ""
            echo "6) Single-Processor Mode:"
            echo "   Terminal 1: ./${BUILD_DIR}/matching_engine --tcp ${DEFAULT_PORT} --single-processor"
            echo "   Terminal 2: ./${BUILD_DIR}/tcp_client localhost ${DEFAULT_PORT}"
            echo ""
            echo "7) Multicast Market Data Feed (NEW!):"
            echo "   Terminal 1: ./${BUILD_DIR}/matching_engine --tcp ${DEFAULT_PORT} --multicast ${MULTICAST_GROUP}:${MULTICAST_PORT}"
            echo "   Terminal 2: ./${BUILD_DIR}/multicast_subscriber ${MULTICAST_GROUP} ${MULTICAST_PORT}"
            echo "   Terminal 3: ./${BUILD_DIR}/tcp_client localhost ${DEFAULT_PORT}"
            echo "               > buy IBM 100 50 1"
            echo "               > sell IBM 100 30 2"
            echo ""
            ;;
        valgrind)
            run_valgrind
            ;;
        valgrind-server)
            shift
            run_valgrind_server "$@"
            ;;
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
