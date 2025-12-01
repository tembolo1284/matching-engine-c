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

# Add after the existing CMAKE_C_FLAGS line
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -mcmodel=medium")

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
    src/modes/tcp_mode.c
    src/modes/udp_mode.c
    src/modes/multicast_helpers.c
    src/modes/helpers.c
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
target_compile_options(matching_engine_tests PRIVATE -g -O0 -mcmodel=medium)
target_link_options(matching_engine_tests PRIVATE -mcmodel=medium)

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

# UDP/Binary protocol test
add_custom_target(test-binary
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Binary Protocol Test - UDP Mode"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Starting server - UDP mode, CSV output..."
    COMMAND bash -c "\"${CMAKE_BINARY_DIR}/matching_engine\" --udp 1234 > /tmp/binary_test_output.txt 2>&1 & SERVER_PID=$$! ; sleep 1 ; echo 'Sending binary test messages...' ; \"${CMAKE_BINARY_DIR}/binary_client\" 1234 1 ; sleep 1 ; echo '' ; echo 'Server output:' ; kill $$SERVER_PID 2>/dev/null || true ; wait $$SERVER_PID 2>/dev/null || true ; cat /tmp/binary_test_output.txt ; rm -f /tmp/binary_test_output.txt ; echo '' ; echo 'Binary protocol test complete'"
    DEPENDS matching_engine binary_client
    COMMENT "Running binary protocol test"
)

add_custom_target(test-tcp
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "TCP Multi-Client Test"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Starting server - TCP mode, port 1234..."
    COMMAND bash -c "\"${CMAKE_BINARY_DIR}/matching_engine\" --tcp 1234 2>&1 & SERVER_PID=$$! ; sleep 1 ; echo 'Connecting test clients...' ; \"${CMAKE_BINARY_DIR}/tcp_client\" localhost 1234 & CLIENT_PID=$$! ; sleep 2 ; echo 'Stopping server...' ; kill $$SERVER_PID 2>/dev/null || true ; kill $$CLIENT_PID 2>/dev/null || true ; wait $$SERVER_PID 2>/dev/null || true ; wait $$CLIENT_PID 2>/dev/null || true ; echo '' ; echo 'TCP test complete'"
    DEPENDS matching_engine tcp_client
    COMMENT "Running TCP multi-client test"
)

# Dual-processor symbol routing test
add_custom_target(test-dual-processor
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Dual-Processor Symbol Routing Test"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Testing symbol partitioning: A-M -> P0, N-Z -> P1"
    COMMAND bash -c "\"${CMAKE_BINARY_DIR}/matching_engine\" --tcp 1234 --dual-processor > /tmp/dual_proc_output.txt 2>&1 & SERVER_PID=$$! ; sleep 1 ; echo 'Sending orders to Processor 0 (A-M symbols)...' ; echo -e 'N,1,IBM,100,50,B,1\\nN,1,AAPL,150,30,B,2\\nN,1,GOOGL,200,20,B,3\\n' | nc -q1 localhost 1234 2>/dev/null || echo -e 'N,1,IBM,100,50,B,1\\nN,1,AAPL,150,30,B,2\\nN,1,GOOGL,200,20,B,3\\n' | nc localhost 1234 ; sleep 0.5 ; echo 'Sending orders to Processor 1 (N-Z symbols)...' ; echo -e 'N,1,NVDA,300,40,B,4\\nN,1,TSLA,180,25,B,5\\nN,1,ZM,50,100,B,6\\n' | nc -q1 localhost 1234 2>/dev/null || echo -e 'N,1,NVDA,300,40,B,4\\nN,1,TSLA,180,25,B,5\\nN,1,ZM,50,100,B,6\\n' | nc localhost 1234 ; sleep 0.5 ; echo 'Sending flush to both processors...' ; echo -e 'F\\n' | nc -q1 localhost 1234 2>/dev/null || echo -e 'F\\n' | nc localhost 1234 ; sleep 1 ; echo '' ; echo 'Stopping server...' ; kill -TERM $$SERVER_PID 2>/dev/null || true ; sleep 1 ; kill -9 $$SERVER_PID 2>/dev/null || true ; wait $$SERVER_PID 2>/dev/null || true ; echo '' ; echo '==========================================' ; echo 'Server Output (check per-processor stats):' ; echo '==========================================' ; cat /tmp/dual_proc_output.txt ; rm -f /tmp/dual_proc_output.txt ; echo '' ; echo 'Expected: ~3 messages to each processor' ; echo 'Dual-processor test complete'"
    DEPENDS matching_engine
    COMMENT "Running dual-processor symbol routing test"
)

# Single-processor comparison test
add_custom_target(test-single-processor
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Single-Processor Mode Test"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "All symbols route to single processor"
    COMMAND bash -c "\"${CMAKE_BINARY_DIR}/matching_engine\" --tcp 1234 --single-processor > /tmp/single_proc_output.txt 2>&1 & SERVER_PID=$$! ; sleep 1 ; echo 'Sending mixed orders (all to single processor)...' ; echo -e 'N,1,IBM,100,50,B,1\\nN,1,NVDA,200,30,B,2\\nF\\n' | nc -q1 localhost 1234 2>/dev/null || echo -e 'N,1,IBM,100,50,B,1\\nN,1,NVDA,200,30,B,2\\nF\\n' | nc localhost 1234 ; sleep 1 ; echo '' ; echo 'Stopping server...' ; kill -TERM $$SERVER_PID 2>/dev/null || true ; sleep 1 ; kill -9 $$SERVER_PID 2>/dev/null || true ; wait $$SERVER_PID 2>/dev/null || true ; echo '' ; echo 'Server Output:' ; cat /tmp/single_proc_output.txt ; rm -f /tmp/single_proc_output.txt ; echo '' ; echo 'Single-processor test complete'"
    DEPENDS matching_engine
    COMMENT "Running single-processor mode test"
)

# Multicast market data feed test
add_custom_target(test-multicast
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Multicast Market Data Feed Test"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Terminal 1 (this terminal):"
    COMMAND ${CMAKE_COMMAND} -E echo "  Server starts with multicast on 239.255.0.1:5000"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Terminal 2 (run this command):"
    COMMAND ${CMAKE_COMMAND} -E echo "  ${CMAKE_BINARY_DIR}/multicast_subscriber 239.255.0.1 5000"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Terminal 3 (send orders):"
    COMMAND ${CMAKE_COMMAND} -E echo "  ${CMAKE_BINARY_DIR}/tcp_client localhost 1234"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "All subscribers will receive the same market data!"
    COMMAND ${CMAKE_COMMAND} -E echo "Press Ctrl+C to stop"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --tcp 1234 --multicast 239.255.0.1:5000
    DEPENDS matching_engine multicast_subscriber
    COMMENT "Running multicast market data feed test"
)

add_custom_target(test-all
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Running Complete Test Suite"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Step 1/5: Unit Tests"
    COMMAND ${CMAKE_COMMAND} -E echo "------------------------------------------"
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test-unit
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Step 2/5: Binary Protocol Test"
    COMMAND ${CMAKE_COMMAND} -E echo "------------------------------------------"
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test-binary
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Step 3/5: TCP Integration Test"
    COMMAND ${CMAKE_COMMAND} -E echo "------------------------------------------"
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test-tcp
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Step 4/5: Dual-Processor Test"
    COMMAND ${CMAKE_COMMAND} -E echo "------------------------------------------"
    COMMAND ${CMAKE_COMMAND} --build ${CMAKE_BINARY_DIR} --target test-dual-processor
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Step 5/5: Multicast Test (manual - see instructions)"
    COMMAND ${CMAKE_COMMAND} -E echo "------------------------------------------"
    COMMAND ${CMAKE_COMMAND} -E echo "Run './build.sh test-multicast' in separate terminals"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "All automated tests finished"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    DEPENDS matching_engine_tests matching_engine binary_client tcp_client multicast_subscriber
    COMMENT "Running all tests"
)

# ============================================================================
# Client Test Targets
# ============================================================================

add_custom_target(test-client-basic
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Client Test - Basic Scenarios"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND bash -c "\"${CMAKE_BINARY_DIR}/matching_engine\" --tcp 1234 > /tmp/server_output.txt 2>&1 & SERVER_PID=$$! ; sleep 1 ; \"${CMAKE_BINARY_DIR}/matching_engine_client\" --scenario 1 localhost 1234 ; \"${CMAKE_BINARY_DIR}/matching_engine_client\" --scenario 2 localhost 1234 ; \"${CMAKE_BINARY_DIR}/matching_engine_client\" --scenario 3 localhost 1234 ; kill $$SERVER_PID 2>/dev/null ; wait $$SERVER_PID 2>/dev/null || true ; echo 'Test complete'"
    DEPENDS matching_engine matching_engine_client
    COMMENT "Running client basic scenario tests"
)

add_custom_target(test-client-stress
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Client Test - Stress Test (10K orders)"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND bash -c "\"${CMAKE_BINARY_DIR}/matching_engine\" --tcp 1234 > /tmp/server_output.txt 2>&1 & SERVER_PID=$$! ; sleep 1 ; \"${CMAKE_BINARY_DIR}/matching_engine_client\" --scenario 11 localhost 1234 ; kill $$SERVER_PID 2>/dev/null ; wait $$SERVER_PID 2>/dev/null || true ; echo 'Test complete'"
    DEPENDS matching_engine matching_engine_client
    COMMENT "Running client stress test"
)

add_custom_target(test-client-multi
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Client Test - Multi-Symbol (Dual Processor)"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND bash -c "\"${CMAKE_BINARY_DIR}/matching_engine\" --tcp 1234 --dual-processor > /tmp/server_output.txt 2>&1 & SERVER_PID=$$! ; sleep 1 ; \"${CMAKE_BINARY_DIR}/matching_engine_client\" --scenario 30 localhost 1234 ; kill $$SERVER_PID 2>/dev/null ; wait $$SERVER_PID 2>/dev/null || true ; cat /tmp/server_output.txt | grep -E 'Processor|messages' ; echo 'Test complete'"
    DEPENDS matching_engine matching_engine_client
    COMMENT "Running client multi-symbol test"
)

add_custom_target(test-client-multicast
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo "Client Test - With Multicast"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Terminal 1 (this terminal):"
    COMMAND ${CMAKE_COMMAND} -E echo "  Server starts with multicast on 239.255.0.1:5000"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Terminal 2 (run this command):"
    COMMAND ${CMAKE_COMMAND} -E echo "  ${CMAKE_BINARY_DIR}/matching_engine_client --multicast 239.255.0.1:5000 localhost 1234"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Terminal 3 (multicast-only subscriber):"
    COMMAND ${CMAKE_COMMAND} -E echo "  ${CMAKE_BINARY_DIR}/matching_engine_client --multicast-only --multicast 239.255.0.1:5000"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Press Ctrl+C to stop"
    COMMAND ${CMAKE_COMMAND} -E echo "=========================================="
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --tcp 1234 --multicast 239.255.0.1:5000
    DEPENDS matching_engine matching_engine_client
    COMMENT "Running client multicast test"
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
        COMMAND bash -c "valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes \"${CMAKE_BINARY_DIR}/matching_engine\" --tcp 2>&1 & VALGRIND_PID=$$! ; sleep 5 ; echo '' ; echo 'Stopping server...' ; pkill -SIGTERM matching_engine 2>/dev/null || true ; wait $$VALGRIND_PID 2>/dev/null || true ; echo '' ; echo 'Valgrind test complete'"
        DEPENDS matching_engine
        COMMENT "Running valgrind memory leak detection"
    )
elseif(APPLE)
    add_custom_target(valgrind
        COMMAND ${CMAKE_COMMAND} -E echo "Starting memory leak test with macOS leaks tool..."
        COMMAND pkill -9 matching_engine 2>/dev/null || true
        COMMAND sleep 1
        COMMAND bash -c "\"${CMAKE_BINARY_DIR}/matching_engine\" --tcp 2>&1 & SERVER_PID=$$! ; sleep 3 ; echo '' ; echo 'Running leaks...' ; leaks $$SERVER_PID 2>&1 | grep -A 20 -E 'Process|leaked bytes' || true ; echo '' ; echo 'Stopping server...' ; kill -TERM $$SERVER_PID 2>/dev/null || true ; sleep 2 ; kill -9 $$SERVER_PID 2>/dev/null || true ; echo 'Leaks test complete'"
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
    COMMENT "Running matching engine in TCP mode (dual-processor)"
)

add_custom_target(run-tcp
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --tcp
    DEPENDS matching_engine
    COMMENT "Running matching engine in TCP mode (dual-processor)"
)

add_custom_target(run-udp
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --udp
    DEPENDS matching_engine
    COMMENT "Running matching engine in UDP mode"
)

add_custom_target(run-dual
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --tcp --dual-processor
    DEPENDS matching_engine
    COMMENT "Running matching engine in TCP dual-processor mode"
)

add_custom_target(run-single
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --tcp --single-processor
    DEPENDS matching_engine
    COMMENT "Running matching engine in TCP single-processor mode"
)

add_custom_target(run-binary
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --tcp --binary
    DEPENDS matching_engine
    COMMENT "Running matching engine in TCP mode with binary output"
)

add_custom_target(run-binary-decoded
    COMMAND bash -c "\"${CMAKE_BINARY_DIR}/matching_engine\" --tcp --binary 2>/dev/null | \"${CMAKE_BINARY_DIR}/binary_decoder\""
    DEPENDS matching_engine binary_decoder
    COMMENT "Running matching engine with binary output (decoded)"
)

add_custom_target(run-multicast
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --tcp --multicast 239.255.0.1:5000
    DEPENDS matching_engine
    COMMENT "Running matching engine with multicast market data feed"
)

add_custom_target(run-multicast-binary
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine --tcp --binary --multicast 239.255.0.1:5000
    DEPENDS matching_engine
    COMMENT "Running matching engine with binary multicast market data feed"
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

add_custom_target(run-client-multicast
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine_client --multicast 239.255.0.1:5000 localhost 1234
    DEPENDS matching_engine_client
    COMMENT "Running matching engine client with multicast"
)

add_custom_target(run-client-mcast-only
    COMMAND ${CMAKE_BINARY_DIR}/matching_engine_client --multicast-only --multicast 239.255.0.1:5000
    DEPENDS matching_engine_client
    COMMENT "Running multicast-only subscriber"
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
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Processor Modes:"
    COMMAND ${CMAKE_COMMAND} -E echo "  --dual-processor   Symbols A-M -> P0, N-Z -> P1 (default)"
    COMMAND ${CMAKE_COMMAND} -E echo "  --single-processor All symbols -> single processor"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Multicast Support:"
    COMMAND ${CMAKE_COMMAND} -E echo "  --multicast <group>:<port>  Broadcast market data to multicast group"
    COMMAND ${CMAKE_COMMAND} -E echo "  Example: --multicast 239.255.0.1:5000"
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
    COMMAND ${CMAKE_COMMAND} -E echo "  binary_client          - Build binary client tool"
    COMMAND ${CMAKE_COMMAND} -E echo "  tcp_client             - Build TCP client tool"
    COMMAND ${CMAKE_COMMAND} -E echo "  multicast_subscriber   - Build multicast subscriber"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Server Testing:"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-unit              - Run unit tests"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-binary            - UDP binary protocol test"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-tcp               - TCP multi-client test"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-dual-processor    - Dual-processor routing test"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-multicast         - Multicast feed test"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-all               - Run full server test suite"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Client Testing:"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-client-basic      - Run basic client scenarios (1-3)"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-client-stress     - Run 10K order stress test"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-client-multi      - Run multi-symbol test"
    COMMAND ${CMAKE_COMMAND} -E echo "  test-client-multicast  - Run client with multicast"
    COMMAND ${CMAKE_COMMAND} -E echo "  client-scenarios       - List available scenarios"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Running Server:"
    COMMAND ${CMAKE_COMMAND} -E echo "  run                    - Run server (TCP dual-processor)"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-tcp                - Run server (TCP mode)"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-udp                - Run server (UDP mode)"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-multicast          - Run server with multicast"
    COMMAND ${CMAKE_COMMAND} -E echo ""
    COMMAND ${CMAKE_COMMAND} -E echo "Running Client:"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-client             - Run client (interactive)"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-client-verbose     - Run client (verbose)"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-client-multicast   - Run client with multicast"
    COMMAND ${CMAKE_COMMAND} -E echo "  run-client-mcast-only  - Run multicast-only subscriber"
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
message(STATUS "Processors:    Dual (A-M/N-Z) by default")
message(STATUS "Multicast:     Enabled (optional)")
message(STATUS "Client:        matching_engine_client")
message(STATUS "========================================")
