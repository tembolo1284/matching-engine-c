# Makefile for Order Book - Matching Engine (Pure C)
# Replaces CMakeLists.txt

# Compiler and flags
CC = gcc
CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -O3 -march=native -pthread
LDFLAGS = -pthread -lm

# Directories
SRCDIR = src
INCDIR = include
OBJDIR = obj
BINDIR = .

# Target executable
TARGET = $(BINDIR)/run_matching_engine

# Source files
SOURCES = $(SRCDIR)/main.c \
          $(SRCDIR)/order_book.c \
          $(SRCDIR)/matching_engine.c \
          $(SRCDIR)/message_parser.c \
          $(SRCDIR)/message_formatter.c \
          $(SRCDIR)/udp_receiver.c \
          $(SRCDIR)/processor.c \
          $(SRCDIR)/output_publisher.c

# Object files
OBJECTS = $(SOURCES:$(SRCDIR)/%.c=$(OBJDIR)/%.o)

# Header dependencies
HEADERS = $(wildcard $(INCDIR)/*.h)

# Default target
all: directories $(TARGET)

# Create necessary directories
directories:
	@mkdir -p $(OBJDIR) $(BINDIR)

# Link object files to create executable
$(TARGET): $(OBJECTS)
	@echo "Linking $@..."
	$(CC) $(OBJECTS) $(LDFLAGS) -o $@
	@echo "Build complete: $@"

# Compile source files to object files
$(OBJDIR)/%.o: $(SRCDIR)/%.c $(HEADERS)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) -I$(INCDIR) -c $< -o $@

# Clean build artifacts
clean:
	@echo "Cleaning build artifacts..."
	rm -rf $(OBJDIR) $(TARGET)
	@echo "Clean complete"

# Deep clean (including directories)
distclean: clean
	rm -rf $(BINDIR)

# Run the program (default port 1234)
run: $(TARGET)
	./$(TARGET)

# Debug build
debug: CFLAGS = -std=c11 -Wall -Wextra -Wpedantic -g -O0 -pthread -DDEBUG
debug: clean all

# Run with valgrind for memory leak detection
valgrind: debug
	valgrind --leak-check=full --show-leak-kinds=all ./$(TARGET)

# Print configuration
info:
	@echo "==================================================="
	@echo "Matching Engine- Makefile Configuration"
	@echo "==================================================="
	@echo "Compiler:    $(CC)"
	@echo "C Standard:  C11"
	@echo "Flags:       $(CFLAGS)"
	@echo "Link flags:  $(LDFLAGS)"
	@echo "Target:      $(TARGET)"
	@echo "Sources:     $(SOURCES)"
	@echo "==================================================="

# Help
help:
	@echo "Available targets:"
	@echo "  all        - Build the project (default)"
	@echo "  clean      - Remove object files and executable"
	@echo "  distclean  - Remove all build artifacts"
	@echo "  debug      - Build with debug symbols"
	@echo "  run        - Build and run the program"
	@echo "  valgrind   - Run with memory leak detection"
	@echo "  info       - Display build configuration"
	@echo "  help       - Display this help message"

.PHONY: all clean distclean run debug valgrind info help directories
