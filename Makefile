# Makefile for Order Book - Matching Engine (Pure C with TCP Support)

# Compiler and flags
CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O3 -march=native -pthread
CFLAGS_DEBUG = -std=c11 -Wall -Wextra -Wpedantic -g -O0 -pthread -DDEBUG
LDFLAGS = -pthread -lm

# Directories
SRCDIR = src
INCDIR = include
BUILDDIR = build
TESTDIR = tests
TOOLSDIR = tools

# Target executable
TARGET = $(BUILDDIR)/matching_engine

# Test executable
TEST_TARGET = $(BUILDDIR)/matching_engine_tests

# Binary tools (now in build directory)
BINARY_CLIENT = $(BUILDDIR)/binary_client
BINARY_DECODER = $(BUILDDIR)/binary_decoder
TCP_CLIENT = $(BUILDDIR)/tcp_client

# Source files (excluding main.c for library)
LIB_SOURCES = $(SRCDIR)/core/order_book.c \
              $(SRCDIR)/core/matching_engine.c \
              $(SRCDIR)/protocol/csv/message_parser.c \
              $(SRCDIR)/protocol/csv/message_formatter.c \
              $(SRCDIR)/protocol/binary/binary_message_parser.c \
              $(SRCDIR)/protocol/binary/binary_message_formatter.c \
              $(SRCDIR)/network/message_framing.c \
              $(SRCDIR)/network/tcp_connection.c \
              $(SRCDIR)/network/tcp_listener.c \
              $(SRCDIR)/network/udp_receiver.c \
              $(SRCDIR)/threading/lockfree_queue.c \
              $(SRCDIR)/threading/queues.c \
              $(SRCDIR)/threading/processor.c \
              $(SRCDIR)/threading/output_publisher.c \
              $(SRCDIR)/threading/output_router.c

# Main source
MAIN_SOURCE = $(SRCDIR)/main.c

# Unity framework
UNITY_SOURCES = $(TESTDIR)/unity.c

# Test sources
TEST_SOURCES = $(TESTDIR)/core/test_order_book.c \
               $(TESTDIR)/protocol/test_message_parser.c \
               $(TESTDIR)/protocol/test_message_formatter.c \
               $(TESTDIR)/protocol/test_matching_engine.c \
               $(TESTDIR)/scenarios/test_scenarios_odd.c \
               $(TESTDIR)/scenarios/test_scenarios_even.c \
               $(TESTDIR)/test_runner.c

# Object files
LIB_OBJECTS = $(LIB_SOURCES:$(SRCDIR)/%.c=$(BUILDDIR)/obj/%.o)
MAIN_OBJECT = $(MAIN_SOURCE:$(SRCDIR)/%.c=$(BUILDDIR)/obj/%.o)
UNITY_OBJECTS = $(UNITY_SOURCES:$(TESTDIR)/%.c=$(BUILDDIR)/obj/tests/%.o)
TEST_OBJECTS = $(TEST_SOURCES:$(TESTDIR)/%.c=$(BUILDDIR)/obj/tests/%.o)

# Header dependencies
HEADERS = $(wildcard $(INCDIR)/*.h) \
          $(wildcard $(INCDIR)/*/*.h) \
          $(wildcard $(INCDIR)/*/*/*.h)

# Default target - build everything including tools
all: directories $(TARGET) tools
	@echo ""
	@echo "✓ Build complete!"
	@echo "  Main executable: $(TARGET)"
	@echo "  Tools:           $(BINARY_CLIENT), $(BINARY_DECODER), $(TCP_CLIENT)"

# Create necessary directories
directories:
	@mkdir -p $(BUILDDIR)/obj/core \
	          $(BUILDDIR)/obj/protocol/csv \
	          $(BUILDDIR)/obj/protocol/binary \
	          $(BUILDDIR)/obj/network \
	          $(BUILDDIR)/obj/threading \
	          $(BUILDDIR)/obj/tests/core \
	          $(BUILDDIR)/obj/tests/protocol \
	          $(BUILDDIR)/obj/tests/scenarios

# Link object files to create executable
$(TARGET): $(LIB_OBJECTS) $(MAIN_OBJECT)
	@echo "Linking $@..."
	$(CC) $(LIB_OBJECTS) $(MAIN_OBJECT) $(LDFLAGS) -o $@

# Compile source files to object files
$(BUILDDIR)/obj/%.o: $(SRCDIR)/%.c $(HEADERS)
	@echo "Compiling $<..."
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -I$(INCDIR) -c $< -o $@

# Build all tools
tools: $(BINARY_CLIENT) $(BINARY_DECODER) $(TCP_CLIENT)

$(BINARY_CLIENT): $(TOOLSDIR)/binary_client.c
	@echo "Building binary client..."
	$(CC) $(CFLAGS) $< -o $@

$(BINARY_DECODER): $(TOOLSDIR)/binary_decoder.c
	@echo "Building binary decoder..."
	$(CC) $(CFLAGS) $< -o $@

$(TCP_CLIENT): $(TOOLSDIR)/tcp_client.c
	@echo "Building TCP test client..."
	$(CC) $(CFLAGS) -I$(INCDIR) $< -o $@

# Test target
test: directories $(TEST_TARGET)
	@echo ""
	@echo "=========================================="
	@echo "Running Unity Tests"
	@echo "=========================================="
	@./$(TEST_TARGET)

# Build test executable
$(TEST_TARGET): $(LIB_OBJECTS) $(UNITY_OBJECTS) $(TEST_OBJECTS)
	@echo "Linking test executable..."
	$(CC) $(LIB_OBJECTS) $(UNITY_OBJECTS) $(TEST_OBJECTS) $(LDFLAGS) -o $@
	@echo "Test build complete: $@"

# Compile test files
$(BUILDDIR)/obj/tests/%.o: $(TESTDIR)/%.c $(HEADERS) $(TESTDIR)/unity.h
	@echo "Compiling test $<..."
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS_DEBUG) -I$(INCDIR) -I$(TESTDIR) -c $< -o $@

# Binary protocol tests (UDP mode with binary)
test-binary: $(TARGET) $(BINARY_CLIENT)
	@echo ""
	@echo "=========================================="
	@echo "Binary Protocol Test - UDP Mode"
	@echo "=========================================="
	@echo "Starting server (UDP mode, CSV output)..."
	@./$(TARGET) --udp 1234 > /tmp/binary_test_output.txt 2>&1 & \
	SERVER_PID=$$!; \
	sleep 1; \
	echo "Sending binary test messages..."; \
	./$(BINARY_CLIENT) 1234 1; \
	sleep 1; \
	echo ""; \
	echo "Server output:"; \
	kill $$SERVER_PID 2>/dev/null; \
	wait $$SERVER_PID 2>/dev/null; \
	cat /tmp/binary_test_output.txt; \
	rm -f /tmp/binary_test_output.txt; \
	echo ""; \
	echo "Binary protocol test complete"

# TCP mode tests
test-tcp: $(TARGET) $(TCP_CLIENT)
	@echo ""
	@echo "=========================================="
	@echo "TCP Multi-Client Test"
	@echo "=========================================="
	@echo "Starting server (TCP mode, port 1234)..."
	@./$(TARGET) --tcp 1234 2>&1 & \
	SERVER_PID=$$!; \
	sleep 1; \
	echo "Connecting test clients..."; \
	./$(TCP_CLIENT) localhost 1234 & \
	CLIENT_PID=$$!; \
	sleep 2; \
	echo "Stopping server..."; \
	kill $$SERVER_PID 2>/dev/null; \
	kill $$CLIENT_PID 2>/dev/null; \
	wait $$SERVER_PID 2>/dev/null; \
	wait $$CLIENT_PID 2>/dev/null; \
	echo ""; \
	echo "TCP test complete"

# Run all tests
test-all: test test-binary test-tcp
	@echo ""
	@echo "=========================================="
	@echo "All Tests Complete"
	@echo "=========================================="

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILDDIR)
	@rm -f /tmp/binary_test_*.txt
	@echo "Clean complete"

# Run the program in UDP mode (default)
run-udp: $(TARGET)
	@echo "Starting matching engine in UDP mode on port 1234..."
	@./$(TARGET) --udp

# Run the program in TCP mode (new default)
run: $(TARGET)
	@echo "Starting matching engine in TCP mode on port 1234..."
	@./$(TARGET) --tcp

run-tcp: $(TARGET)
	@echo "Starting matching engine in TCP mode on port 1234..."
	@./$(TARGET) --tcp

# Run with binary protocol output
run-binary: $(TARGET)
	@echo "Starting matching engine in TCP mode with binary output..."
	@./$(TARGET) --tcp --binary

# Run with binary output and decoder
run-binary-decoded: $(TARGET) $(BINARY_DECODER)
	@echo "Starting matching engine with binary output (decoded)..."
	@./$(TARGET) --tcp --binary 2>/dev/null | ./$(BINARY_DECODER)

# Run with custom port (TCP)
run-port: $(TARGET)
	@echo "Usage: make run-port PORT=5000"
	@./$(TARGET) --tcp $(PORT)

# Run with custom port (UDP)
run-udp-port: $(TARGET)
	@echo "Usage: make run-udp-port PORT=5000"
	@./$(TARGET) --udp $(PORT)

# Debug build
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: clean all

# Run with valgrind for memory leak detection
valgrind: debug
	@echo "Starting valgrind test (will auto-stop after 5 seconds)..."
	@valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET) --tcp 2>&1 & \
	sleep 5; \
	echo ""; \
	echo "Sending shutdown signal..."; \
	pkill -SIGTERM matching_engine 2>/dev/null || true; \
	sleep 1; \
	echo "Valgrind test complete"

# Valgrind tests
valgrind-test: directories $(TEST_TARGET)
	@echo "Starting valgrind test (will auto-stop after 5 seconds)..."
	@valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET) --tcp 2>&1 & \
	SERVER_PID=$$!; \
	sleep 5; \
	echo ""; \
	echo "Sending shutdown signal..."; \
	kill -SIGTERM $$SERVER_PID 2>/dev/null || true; \
	wait $$SERVER_PID 2>/dev/null || true; \
	echo ""; \
	echo "Valgrind test complete"

# Print configuration
info:
	@echo "=========================================="
	@echo "Matching Engine - Build Configuration"
	@echo "=========================================="
	@echo "Compiler:       $(CC)"
	@echo "C Standard:     C11"
	@echo "Flags:          $(CFLAGS)"
	@echo "Link flags:     $(LDFLAGS)"
	@echo "Target:         $(TARGET)"
	@echo "Test target:    $(TEST_TARGET)"
	@echo "Tools:          $(BINARY_CLIENT), $(BINARY_DECODER), $(TCP_CLIENT)"
	@echo "Build dir:      $(BUILDDIR)"
	@echo "Sources:        $(words $(LIB_SOURCES)) files"
	@echo "Tests:          $(words $(TEST_SOURCES)) files"
	@echo "=========================================="

# Help
help:
	@echo "Available targets:"
	@echo "  all              - Build matching engine + tools (default)"
	@echo "  tools            - Build binary/TCP test clients only"
	@echo "  test             - Build and run unit tests"
	@echo "  test-binary      - Test binary protocol (UDP mode)"
	@echo "  test-tcp         - Test TCP multi-client mode"
	@echo "  test-all         - Run all tests (unit + binary + TCP)"
	@echo "  clean            - Remove all build artifacts"
	@echo "  debug            - Build with debug symbols"
	@echo ""
	@echo "Running:"
	@echo "  run              - Run in TCP mode on port 1234"
	@echo "  run-tcp          - Run in TCP mode on port 1234"
	@echo "  run-udp          - Run in UDP mode on port 1234 (legacy)"
	@echo "  run-binary       - Run TCP with binary output"
	@echo "  run-binary-decoded - Run TCP with binary output + decoder"
	@echo "  run-port         - Run TCP on custom port (e.g., make run-port PORT=5000)"
	@echo "  run-udp-port     - Run UDP on custom port"
	@echo ""
	@echo "Analysis:"
	@echo "  valgrind         - Run with memory leak detection"
	@echo "  valgrind-test    - Run tests with valgrind"
	@echo "  info             - Display build configuration"
	@echo "  help             - Display this help message"

.PHONY: all clean run run-tcp run-udp run-binary run-binary-decoded run-port \
        run-udp-port debug valgrind valgrind-test info help directories \
        test test-binary test-tcp test-all tools
