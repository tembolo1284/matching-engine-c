#!/bin/bash
# build.sh - Build script for Matching Engine
# Simplified version - core functionality only

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

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

# Function to print colored output
print_status() {
    echo -e "${BLUE}==>${NC} $1"
}

print_success() {
    echo -e "${GREEN}✓${NC} $1"
}

print_error() {
    echo -e "${RED}✗${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}!${NC} $1"
}

# Function to check if a command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to detect best generator
detect_generator() {
    if command_exists ninja; then
        echo "Ninja"
    else
        echo "Unix Makefiles"
    fi
}

# Function to configure CMake
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

# Function to build targets
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

# Function to clean build directory
clean() {
    if [ -d "$BUILD_DIR" ]; then
        print_status "Cleaning build directory..."
        rm -rf "$BUILD_DIR"
        print_success "Clean complete"
    else
        print_warning "Build directory does not exist"
    fi
}

# Function to run tests
run_tests() {
    local test_type=$1
    
    case $test_type in
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
            print_error "Unknown test type: $test_type"
            exit 1
            ;;
    esac
}

# Function to run valgrind/memory leak detection
run_valgrind() {
    if [ "$PLATFORM" = "Linux" ]; then
        if ! command_exists valgrind; then
            print_error "valgrind not found. Install with: sudo apt-get install valgrind"
            exit 1
        fi
        print_status "Running valgrind memory leak detection..."
    elif [ "$PLATFORM" = "macOS" ]; then
        print_status "Running leaks memory leak detection..."
    else
        print_error "Memory leak detection not supported on $PLATFORM"
        exit 1
    fi
    
    cmake --build "$BUILD_DIR" --target valgrind
}

# Function to run the server
run_server() {
    local args=("$@")
    
    if [ ! -f "$BUILD_DIR/matching_engine" ]; then
        print_error "matching_engine not built. Run: ./build.sh build first"
        exit 1
    fi
    
    print_status "Starting matching engine with args: ${args[*]}"
    "$BUILD_DIR/matching_engine" "${args[@]}"
}

# Function to show build info
show_info() {
    cmake --build "$BUILD_DIR" --target info
}

# Function to show help
show_help() {
    cat << EOF
${GREEN}build.sh${NC} - Matching Engine Build Script

${YELLOW}USAGE:${NC}
    ./build.sh [COMMAND] [OPTIONS]

${YELLOW}COMMANDS:${NC}

  ${GREEN}Building:${NC}
    build                    Clean build with Release configuration
    debug                    Clean build with Debug configuration
    release                  Clean build with Release configuration
    rebuild                  Clean and rebuild
    configure                Configure CMake without building
    clean                    Remove build directory

  ${GREEN}Testing:${NC}
    test                     Run unit tests
    test-binary              Run binary protocol tests
    test-tcp                 Run TCP integration tests
    test-all                 Run all tests
    valgrind                 Run with memory leak detection

  ${GREEN}Running:${NC}
    run [args]               Run server with optional arguments
    run-tcp                  Run server in TCP mode (port 1234)
    run-udp                  Run server in UDP mode (port 1234)
    run-binary               Run server in TCP mode with binary output

  ${GREEN}Tools:${NC}
    info                     Show build configuration
    help                     Show this help message

${YELLOW}EXAMPLES:${NC}

  ${GREEN}# Quick start - build everything${NC}
  ./build.sh build

  ${GREEN}# Build and run unit tests${NC}
  ./build.sh build
  ./build.sh test

  ${GREEN}# Debug build${NC}
  ./build.sh debug

  ${GREEN}# Build and run server on custom port${NC}
  ./build.sh build
  ./build.sh run --tcp 5000

  ${GREEN}# Run all tests${NC}
  ./build.sh test-all

  ${GREEN}# Clean rebuild${NC}
  ./build.sh clean
  ./build.sh build

${YELLOW}PLATFORM:${NC}
  Detected: $PLATFORM
  Default generator: $(detect_generator)

EOF
}

# Main script logic
main() {
    # No arguments - show help
    if [ $# -eq 0 ]; then
        show_help
        exit 0
    fi
    
    # Auto-detect generator if not specified
    GENERATOR=$(detect_generator)
    
    # Parse arguments
    case $1 in
        build)
            BUILD_TYPE="Release"
            clean
            configure "$BUILD_TYPE" "$GENERATOR"
            build
            print_success "Build complete! Executables are in $BUILD_DIR/"
            ;;
        debug)
            BUILD_TYPE="Debug"
            clean
            configure "$BUILD_TYPE" "$GENERATOR"
            build
            print_success "Debug build complete!"
            ;;
        release)
            BUILD_TYPE="Release"
            clean
            configure "$BUILD_TYPE" "$GENERATOR"
            build
            print_success "Release build complete!"
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
            echo "Run './build.sh help' for usage information"
            exit 1
            ;;
    esac
}

# Check for required tools
if ! command_exists cmake; then
    print_error "CMake not found. Please install CMake 3.15 or later."
    exit 1
fi

# Run main function
main "$@"
