# ModemBridge Makefile
# C-based Dialup Modem to Telnet Bridge Server

# Compiler and flags
CC = gcc
CFLAGS = -Wall -Wextra -Werror -O2 -std=gnu11 -D_GNU_SOURCE
LDFLAGS = -pthread
LIBS =

# Directories
SRC_DIR = src
INC_DIR = include
BUILD_DIR = build
OBJ_DIR = obj

# Target executable
TARGET = $(BUILD_DIR)/modembridge

# Source files (base list without telnet.c)
SOURCES = $(SRC_DIR)/main.c $(SRC_DIR)/bridge.c $(SRC_DIR)/serial.c \
          $(SRC_DIR)/modem.c $(SRC_DIR)/config.c $(SRC_DIR)/common.c $(SRC_DIR)/datalog.c \
          $(SRC_DIR)/healthcheck.c $(SRC_DIR)/timestamp.c $(SRC_DIR)/echo.c $(SRC_DIR)/util.c

# Objects will be recalculated after SOURCES is finalized
OBJECTS =

# Header dependencies
INCLUDES = -I$(INC_DIR)

# Debug build option
DEBUG ?= 0
ifeq ($(DEBUG), 1)
    CFLAGS += -g -DDEBUG -O0
endif

# Operating mode detection (build mode)
BUILD_MODE ?= level3
ENABLE_LEVEL1 ?= 1
ENABLE_LEVEL2 ?= 1
ENABLE_LEVEL3 ?= 0

ifeq ($(BUILD_MODE), level1)
    ENABLE_LEVEL2 = 0
    ENABLE_LEVEL3 = 0
    ENABLE_LEVEL1 = 1
else ifeq ($(BUILD_MODE), level2)
    ENABLE_LEVEL2 = 1
    ENABLE_LEVEL3 = 0
    ENABLE_LEVEL1 = 1
else ifeq ($(BUILD_MODE), level3)
    ENABLE_LEVEL2 = 1
    ENABLE_LEVEL3 = 1
    ENABLE_LEVEL1 = 1
endif

# Level 1 (serial only) support option
ifeq ($(ENABLE_LEVEL1), 1)
    CFLAGS += -DENABLE_LEVEL1
endif

# Level 2 (telnet) support option
ifeq ($(ENABLE_LEVEL2), 1)
    CFLAGS += -DENABLE_LEVEL2
    SOURCES += $(SRC_DIR)/telnet.c $(SRC_DIR)/telnet_thread.c

    # Enable telnet test functionality only in level2 build mode
    ifeq ($(BUILD_MODE), level2)
        CFLAGS += -DENABLE_TELNET_TEST
        SOURCES += tests/telnet_test.c
    endif
endif

# Level 3 (pipeline management) support option
ifeq ($(ENABLE_LEVEL3), 1)
    CFLAGS += -DENABLE_LEVEL3
    SOURCES += $(SRC_DIR)/level3.c
endif

# Generate object file list from all sources
OBJECTS = $(patsubst $(SRC_DIR)/%.c,$(OBJ_DIR)/%.o,$(filter $(SRC_DIR)/%,$(SOURCES))) \
          $(patsubst tests/%.c,$(OBJ_DIR)/%.o,$(filter tests/%,$(SOURCES)))

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

# Compile test files
$(OBJ_DIR)/%.o: tests/%.c | $(OBJ_DIR)
	@echo "Compiling test $<..."
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

# Level 1 only build (serial/modem only)
.PHONY: level1
level1:
	$(MAKE) BUILD_MODE=level1

# Level 2 build (serial + telnet, default)
.PHONY: level2
level2:
	$(MAKE) BUILD_MODE=level2

# Level 3 build (serial + telnet + pipeline management)
.PHONY: level3
level3:
	$(MAKE) BUILD_MODE=level3

# Show build configuration
.PHONY: config
config:
	@echo "Build Configuration:"
	@echo "  BUILD_MODE:      $(BUILD_MODE)"
	@echo "  DEBUG:           $(DEBUG)"
	@echo "  ENABLE_LEVEL1:   $(ENABLE_LEVEL1)"
	@echo "  ENABLE_LEVEL2:   $(ENABLE_LEVEL2)"
	@echo "  ENABLE_LEVEL3:   $(ENABLE_LEVEL3)"
	@echo "  CFLAGS:          $(CFLAGS)"
	@echo "  SOURCES:         $(SOURCES)"
	@echo "  OBJECTS:         $(OBJECTS)"

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
	@echo "  all       - Build modembridge (default, Level 3)"
	@echo "  clean     - Remove build artifacts"
	@echo "  debug     - Build with debug symbols"
	@echo "  level1    - Build Level 1 (serial/modem only)"
	@echo "  level2    - Build Level 2 (serial + telnet)"
	@echo "  level3    - Build Level 3 (serial + telnet + pipeline)"
	@echo "  config    - Show build configuration"
	@echo "  install   - Install to system (requires root)"
	@echo "  uninstall - Remove from system (requires root)"
	@echo "  run       - Build and run modembridge with default config"
	@echo "  show      - Show Makefile variables"
	@echo "  help      - Show this help message"
	@echo ""
	@echo "Build Modes:"
	@echo "  BUILD_MODE=level1   - Serial/modem only (no telnet)"
	@echo "  BUILD_MODE=level2   - Serial + telnet"
	@echo "  BUILD_MODE=level3   - Serial + telnet + pipeline management (default)"
	@echo ""
	@echo "Options:"
	@echo "  DEBUG=1             - Enable debug build"
	@echo "  BUILD_MODE=level1    - Build Level 1 mode"
	@echo "  BUILD_MODE=level2    - Build Level 2 mode"
	@echo "  BUILD_MODE=level3    - Build Level 3 mode"
