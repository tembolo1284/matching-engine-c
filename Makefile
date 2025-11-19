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

# Target executable
TARGET = $(BUILDDIR)/matching_engine

# Test executable
TEST_TARGET = $(BUILDDIR)/matching_engine_tests

# Source files (excluding main.c for library)
LIB_SOURCES = $(SRCDIR)/order_book.c \
              $(SRCDIR)/matching_engine.c \
              $(SRCDIR)/message_parser.c \
              $(SRCDIR)/message_formatter.c \
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
               $(TESTDIR)/test_scenarios.c \
               $(TESTDIR)/test_runner.c

# Object files
LIB_OBJECTS = $(LIB_SOURCES:$(SRCDIR)/%.c=$(BUILDDIR)/obj/%.o)
MAIN_OBJECT = $(MAIN_SOURCE:$(SRCDIR)/%.c=$(BUILDDIR)/obj/%.o)
UNITY_OBJECTS = $(UNITY_SOURCES:$(TESTDIR)/%.c=$(BUILDDIR)/obj/tests/%.o)
TEST_OBJECTS = $(TEST_SOURCES:$(TESTDIR)/%.c=$(BUILDDIR)/obj/tests/%.o)

# Header dependencies
HEADERS = $(wildcard $(INCDIR)/*.h)

# Default target
all: directories $(TARGET)

# Create necessary directories
directories:
	@mkdir -p $(BUILDDIR)/obj $(BUILDDIR)/obj/tests

# Link object files to create executable
$(TARGET): $(LIB_OBJECTS) $(MAIN_OBJECT)
	@echo "Linking $@..."
	$(CC) $(LIB_OBJECTS) $(MAIN_OBJECT) $(LDFLAGS) -o $@
	@echo "✓ Build complete: $@"

# Compile source files to object files
$(BUILDDIR)/obj/%.o: $(SRCDIR)/%.c $(HEADERS)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -I$(INCDIR) -c $< -o $@

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
	$(CC) $(CFLAGS_DEBUG) -I$(INCDIR) -I$(TESTDIR) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILDDIR)
	@echo "Clean complete"

# Run the program (default port 1234)
run: $(TARGET)
	@echo "Starting matching engine on port 1234..."
	@./$(TARGET)

# Run with custom port
run-port: $(TARGET)
	@echo "Usage: make run-port PORT=5000"
	@./$(TARGET) $(PORT)

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
	@echo "Build dir:      $(BUILDDIR)"
	@echo "Sources:        $(words $(LIB_SOURCES)) files"
	@echo "Tests:          $(words $(TEST_SOURCES)) files"
	@echo "=========================================="

# Help
help:
	@echo "Available targets:"
	@echo "  all          - Build the matching engine (default)"
	@echo "  test         - Build and run all tests"
	@echo "  clean        - Remove all build artifacts"
	@echo "  debug        - Build with debug symbols"
	@echo "  run          - Build and run on port 1234"
	@echo "  run-port     - Run on custom port (e.g., make run-port PORT=5000)"
	@echo "  valgrind     - Run with memory leak detection"
	@echo "  valgrind-test- Run tests with valgrind"
	@echo "  info         - Display build configuration"
	@echo "  help         - Display this help message"

.PHONY: all clean run run-port debug valgrind valgrind-test info help directories test
