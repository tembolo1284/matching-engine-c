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

# Platform-specific flags
if(UNIX AND NOT APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=native")
elseif(APPLE)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-deprecated-declarations")
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
    src/threading/lockfree_queue.c
    src/threading/queues.c
    src/threading/processor.c
    src/threading/output_publisher.c
    src/threading/output_router.c
)

# Create static library from core sources
add_library(matching_engine_lib STATIC ${LIB_SOURCES})
target_link_libraries(matching_engine_lib PRIVATE pthread m)

# Main executable
add_executable(matching_engine src/main.c)
target_link_libraries(matching_engine PRIVATE matching_engine_lib pthread m)

# Tools
add_executable(binary_client tools/binary_client.c)
target_link_libraries(binary_client PRIVATE pthread)

add_executable(binary_decoder tools/binary_decoder.c)

add_executable(tcp_client tools/tcp_client.c)
target_include_directories(tcp_client PRIVATE include)

# Unity test framework
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
target_compile_options(matching_engine_tests PRIVATE -g -O0)

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

add_custom_target(test-binary
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Binary Protocol Test - UDP Mode"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Starting server - UDP mode, CSV output..."
    COMMAND bash -c "${CMAKE_BINARY_DIR}/matching_engine --udp 1234 > /tmp/binary_test_output.txt 2>&1 & SERVER_PID=$$! && sleep 1 && echo 'Sending binary test messages...' && ${CMAKE_BINARY_DIR}/binary_client 1234 1 && sleep 1 && echo '' && echo 'Server output:' && kill $$SERVER_PID 2>/dev/null || true && wait $$SERVER_PID 2>/dev/null || true && cat /tmp/binary_test_output.txt && rm -f /tmp/binary_test_output.txt && echo '' && echo 'Binary protocol test complete'"
    DEPENDS matching_engine binary_client
    COMMENT "Running binary protocol test"
)

add_custom_target(test-tcp
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "TCP Multi-Client Test"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Starting server - TCP mode, port 1234..."
    COMMAND bash -c "${CMAKE_BINARY_DIR}/matching_engine --tcp 1234 2>&1 & SERVER_PID=$$! && sleep 1 && echo 'Connecting test clients...' && ${CMAKE_BINARY_DIR}/tcp_client localhost 1234 & CLIENT_PID=$$! && sleep 2 && echo 'Stopping server...' && kill $$SERVER_PID 2>/dev/null || true && kill $$CLIENT_PID 2>/dev/null || true && wait $$SERVER_PID 2>/dev/null || true && wait $$CLIENT_PID 2>/dev/null || true && echo '' && echo 'TCP test complete'"
    DEPENDS matching_engine tcp_client
    COMMENT "Running TCP multi-client test"
)

add_custom_target(test-all
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Running Complete Test Suite"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Step 1/3: Unit Tests"
    COMMAND ${CMAKE_COMMAND} -E echo "------------------------------------------"
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test-unit
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Step 2/3: Binary Protocol Test"
    COMMAND ${CMAKE_COMMAND} -E echo "------------------------------------------"
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test-binary
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Step 3/3: TCP Integration Test"
    COMMAND ${CMAKE_COMMAND} -E echo "------------------------------------------"
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test-tcp
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "All tests finished"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    DEPENDS matching_engine_tests matching_engine binary_client tcp_client
    COMMENT "Running all tests"
)

# ============================================================================
# Memory Analysis Targets
# ============================================================================

if(UNIX AND NOT APPLE)
    add_custom_target(valgrind
        COMMAND ${CMAKE_COMMAND} -E echo "Starting valgrind test - auto-stop after 5 seconds..."
        COMMAND pkill -9 matching_engine 2>/dev/null || true
        COMMAND sleep 1
        COMMAND bash -c "valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ${CMAKE_BINARY_DIR}/matching_engine --tcp 2>&1 & VALGRIND_PID=$$! && sleep 5 && echo '' && echo 'Stopping server...' && pkill -SIGTERM matching_engine 2>/dev/null || true && wait $$VALGRIND_PID 2>/dev/null || true && echo '' && echo 'Valgrind test complete'"
        DEPENDS matching_engine
        COMMENT "Running valgrind memory leak detection"
    )
elseif(APPLE)
    add_custom_target(valgrind
        COMMAND ${CMAKE_COMMAND} -E echo "Starting memory leak test with macOS leaks tool..."
        COMMAND pkill -9 matching_engine 2>/dev/null || true
        COMMAND sleep 1
        COMMAND bash -c "${CMAKE_BINARY_DIR}/matching_engine --tcp 2>&1 & SERVER_PID=$$! && sleep 3 && echo '' && echo 'Running leaks...' && leaks $$SERVER_PID 2>&1 | grep -A 20 '^Process\\|leaked bytes' || true && echo '' && echo 'Stopping server...' && kill -TERM $$SERVER_PID 2>/dev/null || true && sleep 2 && kill -9 $$SERVER_PID 2>/dev/null || true && echo 'Leaks test complete'"
        DEPENDS matching_engine
        COMMENT "Running leaks memory leak detection"
    )
endif()

# ============================================================================
# Run Targets
# ============================================================================

add_custom_target(run
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --tcp
    DEPENDS matching_engine
    COMMENT "Running matching engine in TCP mode"
)

add_custom_target(run-tcp
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --tcp
    DEPENDS matching_engine
    COMMENT "Running matching engine in TCP mode"
)

add_custom_target(run-udp
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --udp
    DEPENDS matching_engine
    COMMENT "Running matching engine in UDP mode"
)

add_custom_target(run-binary
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --tcp --binary
    DEPENDS matching_engine
    COMMENT "Running matching engine in TCP mode with binary output"
)

add_custom_target(run-binary-decoded
    COMMAND bash -c "${CMAKE_BINARY_DIR}/matching_engine --tcp --binary 2>/dev/null | ${CMAKE_BINARY_DIR}/binary_decoder"
    DEPENDS matching_engine binary_decoder
    COMMENT "Running matching engine with binary output (decoded)"
)

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
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMENT "Displaying build configuration"
)

add_custom_target(help-targets
    COMMAND ${CMAKE_COMMAND} -E echo "Available targets:"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Building:"
    COMMAND ${CMAKE_COMMAND} -E echo "  all                  - Build matching engine + tools (default)"
    COMMAND ${CMAKE_COMMAND} -E echo "  matching_engine      - Build main executable only"
    COMMAND ${CMAKE_COMMAND} -E echo "  binary_client        - Build binary client tool"
    COMMAND ${CMAKE_COMMAND} -E echo "  binary_decoder       - Build binary decoder tool"
    COMMAND ${CMAKE_COMMAND} -E echo "  tcp_client           - Build TCP client tool"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Testing:"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-unit            - Run unit tests"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-binary          - Test binary protocol (UDP mode)"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-tcp             - Test TCP multi-client mode"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-all             - Run full test suite"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Running:"
    COMMAND ${CMAKE_COMMAND} -E echo "  run                  - Run in TCP mode"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-tcp              - Run in TCP mode"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-udp              - Run in UDP mode"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-binary           - Run TCP with binary output"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-binary-decoded   - Run TCP with binary output + decoder"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Memory Analysis:"
    COMMAND ${CMAKE_COMMAND} -E echo "  valgrind             - Run with memory leak detection"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Information:"
    COMMAND ${CMAKE_COMMAND} -E echo "  info                 - Display build configuration"
    COMMAND ${CMAKE_COMMAND} -E echo "  help-targets         - Display this help message"
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
message(STATUS "========================================")

