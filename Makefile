# ModemBridge Makefile
# C-based Dialup Modem to Telnet Bridge Server

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -Werror -O2 -std=gnu11 -D_GNU_SOURCE
LDFLAGS =
LIBS =

# Directories
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
OBJ_DIR = obj

# Target executable
TARGET = $(BUILD_DIR)/modembridge

# Source files
SOURCES = $(SRC_DIR)/main.c $(SRC_DIR)/bridge.c $(SRC_DIR)/telnet.c $(SRC_DIR)/serial.c \
          $(SRC_DIR)/modem.c $(SRC_DIR)/config.c $(SRC_DIR)/common.c $(SRC_DIR)/datalog.c \
          $(SRC_DIR)/healthcheck.c
OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(SOURCES))

# Header dependencies
INCLUDES = -I$(INC_DIR)

# Debug build option
DEBUG ?= 0
ifeq ($(DEBUG), 1)
    CFLAGS += -g -DDEBUG -O0
endif

# Default target
.PHONY: all
all: $(TARGET)

# Create directories
$(OBJ_DIR):
	@mkdir -p $(OBJ_DIR)

$(BUILD_DIR):
	@mkdir -p $(BUILD_DIR)

# Link modembridge
$(TARGET): $(BUILD_DIR) $(OBJ_DIR) $(OBJECTS)
	@echo "Linking $(TARGET)..."
	$(CC) $(LDFLAGS) $(OBJECTS) $(LIBS) -o $(TARGET)
	@echo "Build complete: $(TARGET)"

# Compile source files
$(OBJ_DIR)/%.o: $(SRC_DIR)/%.c | $(OBJ_DIR)
	@echo "Compiling $<..."
	$(CC) $(CFLAGS) $(INCLUDES) -c $< -o $@

# Clean build artifacts
.PHONY: clean
clean:
	@echo "Cleaning build artifacts..."
	@rm -rf $(BUILD_DIR) $(OBJ_DIR)
	@echo "Clean complete"

# Install (requires root)
.PHONY: install
install: $(TARGET)
	@echo "Installing modembridge..."
	install -m 755 $(TARGET) /usr/local/bin/modembridge
	install -m 644 modembridge.conf /etc/modembridge.conf.example
	@echo "Installation complete"

# Uninstall
.PHONY: uninstall
uninstall:
	@echo "Uninstalling modembridge..."
	rm -f /usr/local/bin/modembridge
	rm -f /etc/modembridge.conf.example
	@echo "Uninstall complete"

# Run
.PHONY: run
run: $(TARGET)
	$(TARGET) -c modembridge.conf

# Debug build
.PHONY: debug
debug:
	$(MAKE) DEBUG=1

# Show variables (for debugging Makefile)
.PHONY: show
show:
	@echo "CC:       $(CC)"
	@echo "CFLAGS:   $(CFLAGS)"
	@echo "SOURCES:  $(SOURCES)"
	@echo "OBJECTS:  $(OBJECTS)"
	@echo "TARGET:   $(TARGET)"

# Help
.PHONY: help
help:
	@echo "ModemBridge Makefile"
	@echo ""
	@echo "Targets:"
	@echo "  all       - Build modembridge (default)"
	@echo "  clean     - Remove build artifacts"
	@echo "  debug     - Build with debug symbols"
	@echo "  install   - Install to system (requires root)"
	@echo "  uninstall - Remove from system (requires root)"
	@echo "  run       - Build and run modembridge with default config"
	@echo "  show      - Show Makefile variables"
	@echo "  help      - Show this help message"
	@echo ""
	@echo "Options:"
	@echo "  DEBUG=1   - Enable debug build"
