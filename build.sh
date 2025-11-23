#!/bin/bash
# build.sh - Build script for Matching Engine

set -e

# Default values
BUILD_DIR="build"
BUILD_TYPE="Release"
GENERATOR="Ninja"

# Detect platform
if [[ "$OSTYPE" == "darwin"* ]]; then
    PLATFORM="macOS"
elif [[ "$OSTYPE" == "linux-gnu"* ]]; then
    PLATFORM="Linux"
else
    PLATFORM="Unknown"
fi

print_status() {
    echo "[STATUS] $1"
}

print_success() {
    echo "[OK] $1"
}

print_error() {
    echo "[ERROR] $1"
}

print_warning() {
    echo "[WARN] $1"
}

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

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
        for target in "${targets[@]}"; do
            cmake --build "$BUILD_DIR" --target "$target"
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

run_tests() {
    local type=$1
    case $type in
        unit)
            print_status "Running unit tests..."
            cmake --build "$BUILD_DIR" --target test-unit
            ;;
        binary)
            print_status "Running binary protocol tests..."
            cmake --build "$BUILD_DIR" --target test-binary
            ;;
        tcp)
            print_status "Running TCP tests..."
            cmake --build "$BUILD_DIR" --target test-tcp
            ;;
        all)
            print_status "Running all tests..."
            cmake --build "$BUILD_DIR" --target test-all
            ;;
        *)
            print_error "Unknown test type: $type"
            exit 1
            ;;
    esac
}

run_valgrind() {
    if [ "$PLATFORM" = "Linux" ]; then
        if ! command_exists valgrind; then
            print_error "valgrind not found. Install with: sudo apt install valgrind"
            exit 1
        fi
        print_status "Running valgrind..."
    elif [ "$PLATFORM" = "macOS" ]; then
        print_status "Running leaks..."
    else
        print_error "Memory check not supported on this platform"
        exit 1
    fi

    cmake --build "$BUILD_DIR" --target valgrind
}

run_server() {
    local args=("$@")

    if [ ! -f "$BUILD_DIR/matching_engine" ]; then
        print_error "matching_engine not built. Run ./build.sh build first."
        exit 1
    fi

    print_status "Starting matching engine..."
    "$BUILD_DIR/matching_engine" "${args[@]}"
}

show_info() {
    cmake --build "$BUILD_DIR" --target info
}

show_help() {
    cat << EOF
Usage:
  ./build.sh [command]

Commands:
  build            Build in Release mode
  debug            Build in Debug mode
  release          Build in Release mode
  rebuild          Clean and rebuild
  configure        Configure CMake
  clean            Remove build directory

Testing:
  test             Run unit tests
  test-binary      Run binary protocol tests
  test-tcp         Run TCP tests
  test-all         Run all tests
  valgrind         Run memory leak detection

Running:
  run [args]       Run server with args
  run-tcp          Run server in TCP mode
  run-udp          Run server in UDP mode
  run-binary       Run server in TCP binary mode

Tools:
  info             Show build info
  help             Show this message

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

    case $1 in
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
            run_tests unit
            ;;
        test-binary)
            run_tests binary
            ;;
        test-tcp)
            run_tests tcp
            ;;
        test-all)
            run_tests all
            ;;
        valgrind)
            run_valgrind
            ;;
        run)
            shift
            run_server "$@"
            ;;
        run-tcp)
            run_server --tcp
            ;;
        run-udp)
            run_server --udp
            ;;
        run-binary)
            run_server --tcp --binary
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

