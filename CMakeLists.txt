cmake_minimum_required(VERSION 3.15)
project(MatchingEngine C)

# Set C standard
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Detect platform
if(APPLE)
    set(PLATFORM "macOS")
elseif(UNIX)
    set(PLATFORM "Linux")
else()
    set(PLATFORM "Unknown")
endif()

# Output directories
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# Compiler flags (basic warnings only)
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -Wpedantic -Werror -pthread")
set(CMAKE_C_FLAGS_RELEASE "-O3")
set(CMAKE_C_FLAGS_DEBUG "-g -O0 -DDEBUG")

# mcmodel=medium is only supported on x86_64 Linux (not macOS, not ARM)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcmodel=medium")
    set(MCMODEL_SUPPORTED TRUE)
else()
    set(MCMODEL_SUPPORTED FALSE)
endif()

# Valgrind-compatible build option (disables AVX-512)
option(VALGRIND_BUILD "Build without AVX-512 for Valgrind compatibility" OFF)

# Platform-specific flags
if(UNIX AND NOT APPLE)
    if(VALGRIND_BUILD)
        # Use baseline x86-64-v3 (AVX2) for Valgrind compatibility
        # Valgrind 3.18 doesn't support AVX-512 instructions
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=x86-64-v3")
        message(STATUS "Valgrind build: using -march=x86-64-v3 (no AVX-512)")
    else()
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")
    endif()
elseif(APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-deprecated-declarations")
    # Apple Silicon uses ARM64 - no special arch flags needed
endif()

# Include directories
include_directories(include)

# Library sources (core functionality)
set(LIB_SOURCES
    src/core/order_book.c
    src/core/matching_engine.c
    src/protocol/csv/message_parser.c
    src/protocol/csv/message_formatter.c
    src/protocol/binary/binary_message_parser.c
    src/protocol/binary/binary_message_formatter.c
    src/network/message_framing.c
    src/network/tcp_connection.c
    src/network/tcp_listener.c
    src/network/udp_receiver.c
    src/network/multicast_publisher.c
    src/threading/lockfree_queue.c
    src/threading/queues.c
    src/threading/processor.c
    src/threading/output_publisher.c
    src/threading/output_router.c
    src/threading/client_registry.c
    src/modes/tcp_mode.c
    src/modes/udp_mode.c
    src/modes/multicast_helpers.c
    src/modes/helpers.c
    src/modes/unified_mode.c
    src/modes/unified_tcp.c
    src/modes/unified_udp.c
    src/modes/unified_router.c
)

# Create static library from core sources
add_library(matching_engine_lib STATIC ${LIB_SOURCES})
target_link_libraries(matching_engine_lib PRIVATE pthread m)

# Main executable (slim dispatcher)
add_executable(matching_engine src/main.c)
target_link_libraries(matching_engine PRIVATE matching_engine_lib pthread m)

# ============================================================================
# Tools
# ============================================================================

add_executable(binary_client tools/binary_client.c)
target_link_libraries(binary_client PRIVATE pthread)

add_executable(binary_decoder tools/binary_decoder.c)

add_executable(tcp_client tools/tcp_client.c)
target_include_directories(tcp_client PRIVATE include)

add_executable(multicast_subscriber tools/multicast_subscriber.c)
target_link_libraries(multicast_subscriber PRIVATE pthread)

# ============================================================================
# Matching Engine Client
# ============================================================================

# Client library sources
set(CLIENT_LIB_SOURCES
    tools/client/transport.c
    tools/client/codec.c
    tools/client/engine_client.c
    tools/client/scenarios.c
    tools/client/interactive.c
)

# Create client library
add_library(matching_engine_client_lib STATIC ${CLIENT_LIB_SOURCES})
target_include_directories(matching_engine_client_lib PRIVATE
    include
    tools
)
target_link_libraries(matching_engine_client_lib PRIVATE
    matching_engine_lib
    pthread
    m
)

# Main client executable
add_executable(matching_engine_client tools/matching_engine_client.c)
target_include_directories(matching_engine_client PRIVATE
    include
    tools
)
target_link_libraries(matching_engine_client PRIVATE
    matching_engine_client_lib
    matching_engine_lib
    pthread
    m
)

# ============================================================================
# Unity Test Framework
# ============================================================================

set(UNITY_SOURCES tests/unity.c)
add_library(unity STATIC ${UNITY_SOURCES})
target_include_directories(unity PUBLIC tests)

# Test sources
set(TEST_SOURCES
    tests/core/test_order_book.c
    tests/protocol/test_message_parser.c
    tests/protocol/test_message_formatter.c
    tests/core/test_matching_engine.c
    tests/scenarios/test_scenarios_odd.c
    tests/scenarios/test_scenarios_even.c
    tests/core/test_memory_pools.c
    tests/threading/test_lockfree_queue.c
    tests/protocol/test_symbol_router.c
    tests/test_runner.c
)

# Test executable
add_executable(matching_engine_tests ${TEST_SOURCES})
target_link_libraries(matching_engine_tests PRIVATE
    matching_engine_lib
    unity
    pthread
    m
)
target_include_directories(matching_engine_tests PRIVATE
    include
    tests
)

# Test-specific compile options (platform-aware)
if(MCMODEL_SUPPORTED)
    target_compile_options(matching_engine_tests PRIVATE -g -O0 -mcmodel=medium)
    target_link_options(matching_engine_tests PRIVATE -mcmodel=medium)
else()
    target_compile_options(matching_engine_tests PRIVATE -g -O0)
endif()

# Enable testing
enable_testing()
add_test(NAME UnitTests COMMAND matching_engine_tests)

# ============================================================================
# Test Targets
# ============================================================================

add_custom_target(test-unit
    COMMAND matching_engine_tests
    DEPENDS matching_engine_tests
    COMMENT "Running Unity unit tests"
)

# ============================================================================
# Run Targets (Unified Server)
# ============================================================================

# Default run - all transports, CSV format, dual processor
add_custom_target(run
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine
    DEPENDS matching_engine
    COMMENT "Running unified server (TCP:1234, UDP:1235, Multicast:1236)"
)

# Binary format
add_custom_target(run-binary
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --binary
    DEPENDS matching_engine
    COMMENT "Running unified server with binary format"
)

# Quiet mode (benchmark)
add_custom_target(run-quiet
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --quiet
    DEPENDS matching_engine
    COMMENT "Running unified server in quiet/benchmark mode"
)

# Binary + quiet (benchmark)
add_custom_target(run-benchmark
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --binary --quiet
    DEPENDS matching_engine
    COMMENT "Running unified server in binary benchmark mode"
)

# Single processor mode
add_custom_target(run-single
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --single-processor
    DEPENDS matching_engine
    COMMENT "Running unified server with single processor"
)

# ============================================================================
# Client Run Targets
# ============================================================================

add_custom_target(run-client
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine_client localhost 1234
    DEPENDS matching_engine_client
    COMMENT "Running matching engine client (interactive mode)"
)

add_custom_target(run-client-verbose
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine_client --verbose localhost 1234
    DEPENDS matching_engine_client
    COMMENT "Running matching engine client (verbose)"
)

add_custom_target(client-scenarios
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine_client --list-scenarios
    DEPENDS matching_engine_client
    COMMENT "Listing available client scenarios"
)

# ============================================================================
# Memory Analysis Targets
# ============================================================================

if(UNIX AND NOT APPLE)
    add_custom_target(valgrind
        COMMAND ${CMAKE_COMMAND} -E echo "Starting valgrind test - auto-stop after 5 seconds..."
        COMMAND pkill -9 matching_engine 2>/dev/null || true
        COMMAND sleep 1
        COMMAND bash -c "valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \"${CMAKE_BINARY_DIR}/matching_engine\" 2>&1 & VALGRIND_PID=$$! ; sleep 5 ; echo '' ; echo 'Stopping server...' ; pkill -SIGTERM matching_engine 2>/dev/null || true ; wait $$VALGRIND_PID 2>/dev/null || true ; echo '' ; echo 'Valgrind test complete'"
        DEPENDS matching_engine
        COMMENT "Running valgrind memory leak detection"
    )
elseif(APPLE)
    add_custom_target(valgrind
        COMMAND ${CMAKE_COMMAND} -E echo "Starting memory leak test with macOS leaks tool..."
        COMMAND pkill -9 matching_engine 2>/dev/null || true
        COMMAND sleep 1
        COMMAND bash -c "\"${CMAKE_BINARY_DIR}/matching_engine\" 2>&1 & SERVER_PID=$$! ; sleep 3 ; echo '' ; echo 'Running leaks...' ; leaks $$SERVER_PID 2>&1 | grep -A 20 -E 'Process|leaked bytes' || true ; echo '' ; echo 'Stopping server...' ; kill -TERM $$SERVER_PID 2>/dev/null || true ; sleep 2 ; kill -9 $$SERVER_PID 2>/dev/null || true ; echo 'Leaks test complete'"
        DEPENDS matching_engine
        COMMENT "Running leaks memory leak detection"
    )
endif()

# ============================================================================
# Information Targets
# ============================================================================

add_custom_target(info
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Matching Engine - Build Configuration"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Platform:       ${PLATFORM}"
    COMMAND ${CMAKE_COMMAND} -E echo "Compiler:       ${CMAKE_C_COMPILER}"
    COMMAND ${CMAKE_COMMAND} -E echo "C Standard:     C11"
    COMMAND ${CMAKE_COMMAND} -E echo "Build type:     ${CMAKE_BUILD_TYPE}"
    COMMAND ${CMAKE_COMMAND} -E echo "C Flags:        ${CMAKE_C_FLAGS}"
    COMMAND ${CMAKE_COMMAND} -E echo "Build dir:      ${CMAKE_BINARY_DIR}"
    COMMAND ${CMAKE_COMMAND} -E echo "mcmodel:        ${MCMODEL_SUPPORTED}"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Unified Server (always starts all transports):"
    COMMAND ${CMAKE_COMMAND} -E echo "  TCP Port:       1234"
    COMMAND ${CMAKE_COMMAND} -E echo "  UDP Port:       1235"
    COMMAND ${CMAKE_COMMAND} -E echo "  Multicast:      239.255.0.1:1236"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Server Options:"
    COMMAND ${CMAKE_COMMAND} -E echo "  --binary            Binary format (default: CSV)"
    COMMAND ${CMAKE_COMMAND} -E echo "  --quiet             Benchmark mode (stats only)"
    COMMAND ${CMAKE_COMMAND} -E echo "  --single-processor  Single processor (default: dual)"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMENT "Displaying build configuration"
)

add_custom_target(help-targets
    COMMAND ${CMAKE_COMMAND} -E echo "Available targets:"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Building:"
    COMMAND ${CMAKE_COMMAND} -E echo "  all                    - Build everything (default)"
    COMMAND ${CMAKE_COMMAND} -E echo "  matching_engine        - Build server executable"
    COMMAND ${CMAKE_COMMAND} -E echo "  matching_engine_client - Build client executable"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Testing:"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-unit              - Run unit tests"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Running Server:"
    COMMAND ${CMAKE_COMMAND} -E echo "  run                    - Run server (CSV, dual processor)"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-binary             - Run server (binary format)"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-quiet              - Run server (quiet/benchmark)"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-benchmark          - Run server (binary + quiet)"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-single             - Run server (single processor)"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Running Client:"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-client             - Run client (interactive)"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-client-verbose     - Run client (verbose)"
    COMMAND ${CMAKE_COMMAND} -E echo "  client-scenarios       - List available scenarios"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Memory Analysis:"
    COMMAND ${CMAKE_COMMAND} -E echo "  valgrind               - Run with memory leak detection"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Information:"
    COMMAND ${CMAKE_COMMAND} -E echo "  info                   - Display build configuration"
    COMMAND ${CMAKE_COMMAND} -E echo "  help-targets           - Display this help message"
    COMMENT "Displaying help"
)

# ============================================================================
# Summary
# ============================================================================

message(STATUS "========================================")
message(STATUS "Matching Engine - CMake Configuration")
message(STATUS "========================================")
message(STATUS "Platform:      ${PLATFORM}")
message(STATUS "Compiler:      ${CMAKE_C_COMPILER}")
message(STATUS "Build type:    ${CMAKE_BUILD_TYPE}")
message(STATUS "C Standard:    C${CMAKE_C_STANDARD}")
message(STATUS "mcmodel:       ${MCMODEL_SUPPORTED}")
message(STATUS "")
message(STATUS "Unified Server Ports:")
message(STATUS "  TCP:         1234")
message(STATUS "  UDP:         1235")
message(STATUS "  Multicast:   239.255.0.1:1236")
message(STATUS "========================================")
