# Makefile for zulu-cow C++ project

# Compiler and flags
CXX = g++
CXXFLAGS = -std=c++17 -Wall -Wextra -pedantic -O2
DEBUG_FLAGS = -g -DDEBUG -O0
RELEASE_FLAGS = -DNDEBUG -O3

# Directories
SRC_DIR = .
OBJ_DIR = obj
BIN_DIR = bin

# Source files
SOURCES = test_main.cpp zulu_cow.cpp
OBJECTS = $(SOURCES:%.cpp=$(OBJ_DIR)/%.o)

# Target executable (change if you want a different name)
TARGET = $(BIN_DIR)/test_zulu_cow

# Default build type
BUILD_TYPE ?= release

# Set flags based on build type
ifeq ($(BUILD_TYPE), debug)
    CXXFLAGS += $(DEBUG_FLAGS)
else
    CXXFLAGS += $(RELEASE_FLAGS)
endif

# Default target
.PHONY: all
all: $(TARGET)

# Create directories if they don't exist
$(OBJ_DIR):
	mkdir -p $(OBJ_DIR)

$(BIN_DIR):
	mkdir -p $(BIN_DIR)

# Build object files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.cpp | $(OBJ_DIR)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Build the main target
$(TARGET): $(OBJECTS) | $(BIN_DIR)
	$(CXX) $(CXXFLAGS) $(OBJECTS) -o $@

# Debug build
.PHONY: debug
debug:
	$(MAKE) BUILD_TYPE=debug

# Release build
.PHONY: release
release:
	$(MAKE) BUILD_TYPE=release

# Clean build artifacts
.PHONY: clean
clean:
	rm -rf $(OBJ_DIR) $(BIN_DIR)

# Clean and rebuild
.PHONY: rebuild
rebuild: clean all

# Install target (optional - adjust paths as needed)
PREFIX ?= /usr/local
.PHONY: install
install: $(TARGET)
	mkdir -p $(PREFIX)/bin
	cp $(TARGET) $(PREFIX)/bin/

# Uninstall target
.PHONY: uninstall
uninstall:
	rm -f $(PREFIX)/bin/$(notdir $(TARGET))

# Show help
.PHONY: help
help:
	@echo "Available targets:"
	@echo "  all      - Build the project (default)"
	@echo "  debug    - Build with debug flags"
	@echo "  release  - Build with release flags"
	@echo "  clean    - Remove build artifacts"
	@echo "  rebuild  - Clean and build"
	@echo "  install  - Install to PREFIX (default: /usr/local)"
	@echo "  uninstall- Remove installed files"
	@echo "  help     - Show this help"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_TYPE - 'debug' or 'release' (default: release)"
	@echo "  PREFIX     - Installation prefix (default: /usr/local)"
	@echo ""
	@echo "Examples:"
	@echo "  make              # Build release version"
	@echo "  make debug        # Build debug version"
	@echo "  make BUILD_TYPE=debug  # Alternative debug build"
	@echo "  make install PREFIX=~/local  # Install to custom location"

# Print build information
.PHONY: info
info:
	@echo "Build Configuration:"
	@echo "  Compiler: $(CXX)"
	@echo "  Flags: $(CXXFLAGS)"
	@echo "  Build Type: $(BUILD_TYPE)"
	@echo "  Sources: $(SOURCES)"
	@echo "  Objects: $(OBJECTS)"
	@echo "  Target: $(TARGET)"
