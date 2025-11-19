# Makefile for Order Book - Matching Engine (Pure C)

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

# Source files (excluding main.c for library)
LIB_SOURCES = $(SRCDIR)/order_book.c \
              $(SRCDIR)/matching_engine.c \
              $(SRCDIR)/message_parser.c \
              $(SRCDIR)/message_formatter.c \
              $(SRCDIR)/binary_message_parser.c \
              $(SRCDIR)/binary_message_formatter.c \
              $(SRCDIR)/lockfree_queue.c \
              $(SRCDIR)/udp_receiver.c \
              $(SRCDIR)/processor.c \
              $(SRCDIR)/output_publisher.c

# Main source
MAIN_SOURCE = $(SRCDIR)/main.c

# Unity framework
UNITY_SOURCES = $(TESTDIR)/unity.c

# Test sources
TEST_SOURCES = $(TESTDIR)/test_order_book.c \
               $(TESTDIR)/test_message_parser.c \
               $(TESTDIR)/test_message_formatter.c \
               $(TESTDIR)/test_matching_engine.c \
               $(TESTDIR)/test_scenarios_odd.c \
               $(TESTDIR)/test_scenarios_even.c \
               $(TESTDIR)/test_runner.c

# Object files
LIB_OBJECTS = $(LIB_SOURCES:$(SRCDIR)/%.c=$(BUILDDIR)/obj/%.o)
MAIN_OBJECT = $(MAIN_SOURCE:$(SRCDIR)/%.c=$(BUILDDIR)/obj/%.o)
UNITY_OBJECTS = $(UNITY_SOURCES:$(TESTDIR)/%.c=$(BUILDDIR)/obj/tests/%.o)
TEST_OBJECTS = $(TEST_SOURCES:$(TESTDIR)/%.c=$(BUILDDIR)/obj/tests/%.o)

# Header dependencies
HEADERS = $(wildcard $(INCDIR)/*.h)

# Default target - build everything including binary tools
all: directories $(TARGET) binary-tools
	@echo ""
	@echo "✓ Build complete!"
	@echo "  Main executable: $(TARGET)"
	@echo "  Binary tools:    $(BINARY_CLIENT), $(BINARY_DECODER)"

# Create necessary directories
directories:
	@mkdir -p $(BUILDDIR)/obj $(BUILDDIR)/obj/tests

# Link object files to create executable
$(TARGET): $(LIB_OBJECTS) $(MAIN_OBJECT)
	@echo "Linking $@..."
	$(CC) $(LIB_OBJECTS) $(MAIN_OBJECT) $(LDFLAGS) -o $@

# Compile source files to object files
$(BUILDDIR)/obj/%.o: $(SRCDIR)/%.c $(HEADERS)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -I$(INCDIR) -c $< -o $@

# Build binary tools
binary-tools: $(BINARY_CLIENT) $(BINARY_DECODER)

$(BINARY_CLIENT): $(TOOLSDIR)/binary_client.c
	@echo "Building binary client..."
	$(CC) $(CFLAGS) $< -o $@

$(BINARY_DECODER): $(TOOLSDIR)/binary_decoder.c
	@echo "Building binary decoder..."
	$(CC) $(CFLAGS) $< -o $@

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
	@echo "✓ Test build complete: $@"

# Compile test files
$(BUILDDIR)/obj/tests/%.o: $(TESTDIR)/%.c $(HEADERS) $(TESTDIR)/unity.h
	@echo "Compiling test $<..."
	@$(CC) $(CFLAGS_DEBUG) -I$(INCDIR) -I$(TESTDIR) -c $< -o $@

# Binary protocol tests
test-binary: $(TARGET) $(BINARY_CLIENT)
	@echo ""
	@echo "=========================================="
	@echo "Binary Protocol Test - CSV Output"
	@echo "=========================================="
	@echo "Starting server (CSV output mode)..."
	@./$(TARGET) 1234 > /tmp/binary_test_output.txt 2>&1 & \
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
	echo "✓ Binary protocol test complete"

test-binary-full: $(TARGET) $(BINARY_CLIENT) $(BINARY_DECODER)
	@echo ""
	@echo "=========================================="
	@echo "Binary Protocol Test - Binary Output"
	@echo "=========================================="
	@echo "Starting server (binary output mode)..."
	@./$(TARGET) 1234 --binary 2>&1 | ./$(BINARY_DECODER) > /tmp/binary_test_decoded.txt & \
	SERVER_PID=$$!; \
	sleep 1; \
	echo "Sending binary test messages..."; \
	./$(BINARY_CLIENT) 1234 2; \
	sleep 1; \
	echo ""; \
	echo "Decoded output:"; \
	kill $$SERVER_PID 2>/dev/null; \
	wait $$SERVER_PID 2>/dev/null; \
	cat /tmp/binary_test_decoded.txt 2>/dev/null || echo "No output captured"; \
	rm -f /tmp/binary_test_decoded.txt; \
	echo ""; \
	echo "✓ Full binary protocol test complete"

# Run all tests (unit + binary)
test-all: test test-binary
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

# Run the program (default port 1234, CSV output)
run: $(TARGET)
	@echo "Starting matching engine on port 1234 (CSV output)..."
	@./$(TARGET)

# Run with binary protocol output
run-binary: $(TARGET)
	@echo "Starting matching engine on port 1234 (BINARY output)..."
	@./$(TARGET) --binary

# Run with binary output and decoder
run-binary-decoded: $(TARGET) $(BINARY_DECODER)
	@echo "Starting matching engine with binary output (decoded)..."
	@./$(TARGET) --binary 2>/dev/null | ./$(BINARY_DECODER)

# Run with custom port
run-port: $(TARGET)
	@echo "Usage: make run-port PORT=5000"
	@./$(TARGET) $(PORT)

# Run with custom port and binary output
run-binary-port: $(TARGET)
	@echo "Usage: make run-binary-port PORT=5000"
	@./$(TARGET) $(PORT) --binary

# Debug build
debug: CFLAGS = $(CFLAGS_DEBUG)
debug: clean all

# Run with valgrind for memory leak detection
valgrind: debug
	valgrind --leak-check=full --show-leak-kinds=all --track-origins=yes ./$(TARGET)

# Valgrind tests
valgrind-test: directories $(TEST_TARGET)
	valgrind --leak-check=full --show-leak-kinds=all ./$(TEST_TARGET)

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
	@echo "Binary tools:   $(BINARY_CLIENT), $(BINARY_DECODER)"
	@echo "Build dir:      $(BUILDDIR)"
	@echo "Sources:        $(words $(LIB_SOURCES)) files"
	@echo "Tests:          $(words $(TEST_SOURCES)) files"
	@echo "=========================================="

# Help
help:
	@echo "Available targets:"
	@echo "  all              - Build matching engine + binary tools (default)"
	@echo "  binary-tools     - Build binary client and decoder only"
	@echo "  test             - Build and run unit tests"
	@echo "  test-binary      - Test binary protocol with CSV output"
	@echo "  test-binary-full - Test full binary protocol (binary in/out)"
	@echo "  test-all         - Run all tests (unit + binary)"
	@echo "  clean            - Remove all build artifacts"
	@echo "  debug            - Build with debug symbols"
	@echo "  run              - Run on port 1234 (CSV output)"
	@echo "  run-binary       - Run on port 1234 (BINARY output)"
	@echo "  run-binary-decoded - Run with binary output + live decoder"
	@echo "  run-port         - Run on custom port (e.g., make run-port PORT=5000)"
	@echo "  run-binary-port  - Run on custom port with binary output"
	@echo "  valgrind         - Run with memory leak detection"
	@echo "  valgrind-test    - Run tests with valgrind"
	@echo "  info             - Display build configuration"
	@echo "  help             - Display this help message"

.PHONY: all clean run run-binary run-binary-decoded run-port run-binary-port \
        debug valgrind valgrind-test info help directories test test-binary \
        test-binary-full test-all binary-tools

