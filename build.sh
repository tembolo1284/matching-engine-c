#!/bin/bash
# build.sh - Build script for Matching Engine (README run-modes)

set -e

BUILD_DIR="build"
BUILD_TYPE="Release"
GENERATOR="Ninja"
DEFAULT_PORT=1234

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

    print_status "Configuring CMake..."
    echo "  Build directory: $BUILD_DIR"
    echo "  Build type: $build_type"
    echo "  Generator: $generator"
    echo "  Platform: $PLATFORM"

    cmake -B "$BUILD_DIR" -G "$generator" -DCMAKE_BUILD_TYPE="$build_type"
    print_success "Configuration complete"
}

build() {
    local targets=("$@")
    if [ ${#targets[@]} -eq 0 ]; then
        print_status "Building all targets..."
        cmake --build "$BUILD_DIR"
    else
        print_status "Building targets: ${targets[*]}..."
        for t in "${targets[@]}"; do
            cmake --build "$BUILD_DIR" --target "$t"
        done
    fi
    print_success "Build complete"
}

clean() {
    if [ -d "$BUILD_DIR" ]; then
        print_status "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
        print_success "Clean complete"
    else
        print_warning "Build directory does not exist"
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

run_unit_tests() {
    print_status "Running unit tests..."
    cmake --build "$BUILD_DIR" --target test-unit
    print_success "Unit tests complete"
}

run_valgrind() {
    if [ "$PLATFORM" != "Linux" ]; then
        print_error "valgrind only supported on Linux here"
        exit 1
    fi
    if ! command_exists valgrind; then
        print_error "valgrind not found. Install with: sudo apt install valgrind"
        exit 1
    fi
    require_built
    print_status "Running valgrind (TCP server). Ctrl+C to stop."
    valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \
        "./${BUILD_DIR}/matching_engine" --tcp "$DEFAULT_PORT"
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

Real Tests:
  test             Run Unity unit tests (C tests)

README Run-Modes (2-terminal workflows):
  test-binary          UDP server CSV output + binary_client scenarios
  test-binary-full     UDP server binary output piped to decoder
  test-tcp             TCP server CSV output + binary_client --tcp
  test-tcp-csv         TCP server CSV output + binary_client --tcp --csv
  test-dual-processor  TCP server dual-processor mode (symbol routing test)
  test-single-processor TCP server single-processor mode (comparison)
  test-all             Prints all README run-modes (does not start anything)
  valgrind             Run valgrind server (Linux)

Scenario examples:
  ./build.sh test-binary 1234 all
  ./build.sh test-binary-full 1234 1 2 3
  ./build.sh test-tcp 1234 2
  ./build.sh test-tcp-csv 1234 2
  ./build.sh test-dual-processor 1234

Run:
  run [args]       Run server directly (same as README)
  run-tcp          Run TCP server on 1234 (dual-processor, default)
  run-udp          Run UDP server on 1234
  run-dual         Run TCP server in dual-processor mode (default)
  run-single       Run TCP server in single-processor mode

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
            ;;
        valgrind)
            run_valgrind
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
