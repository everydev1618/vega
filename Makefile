# Vega Language - Makefile
# Build system for vegac (compiler) and vega (VM)

CC = gcc
CFLAGS = -Wall -Wextra -std=c11 -g -I./src
LDFLAGS =
LDLIBS = -lcurl

# Directories
SRC_DIR = src
BUILD_DIR = build
BIN_DIR = bin

# Compiler sources
COMPILER_SRC = $(SRC_DIR)/compiler/main.c \
               $(SRC_DIR)/compiler/lexer.c \
               $(SRC_DIR)/compiler/parser.c \
               $(SRC_DIR)/compiler/ast.c \
               $(SRC_DIR)/compiler/sema.c \
               $(SRC_DIR)/compiler/codegen.c \
               $(SRC_DIR)/common/memory.c

# VM sources (without TUI)
VM_CORE_SRC = $(SRC_DIR)/vm/vm.c \
              $(SRC_DIR)/vm/value.c \
              $(SRC_DIR)/vm/agent.c \
              $(SRC_DIR)/vm/http.c \
              $(SRC_DIR)/vm/process.c \
              $(SRC_DIR)/vm/scheduler.c \
              $(SRC_DIR)/common/memory.c \
              $(SRC_DIR)/stdlib/file.c \
              $(SRC_DIR)/stdlib/str.c \
              $(SRC_DIR)/stdlib/json.c

# TUI sources
TUI_SRC = $(SRC_DIR)/tui/main.c \
          $(SRC_DIR)/tui/tui.c \
          $(SRC_DIR)/tui/trace.c \
          $(SRC_DIR)/tui/repl.c

# Full VM sources (includes main.c and TUI)
VM_SRC = $(SRC_DIR)/vm/main.c \
         $(VM_CORE_SRC) \
         $(TUI_SRC)

# Object files
COMPILER_OBJ = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(COMPILER_SRC))
VM_OBJ = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(VM_SRC))

# TUI requires ncurses
TUI_LDLIBS = -lncurses

# Targets
VEGAC = $(BIN_DIR)/vegac
VEGA = $(BIN_DIR)/vega

.PHONY: all clean test vegac vega dirs

all: dirs vegac vega

dirs:
	@mkdir -p $(BUILD_DIR)/compiler
	@mkdir -p $(BUILD_DIR)/vm
	@mkdir -p $(BUILD_DIR)/common
	@mkdir -p $(BUILD_DIR)/stdlib
	@mkdir -p $(BUILD_DIR)/tui
	@mkdir -p $(BIN_DIR)

vegac: dirs $(VEGAC)

vega: dirs $(VEGA)

$(VEGAC): $(COMPILER_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(VEGA): $(VM_OBJ)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS) $(TUI_LDLIBS)

# Pattern rule for object files
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

# Dependencies (auto-generated would be better, but this works for now)
$(BUILD_DIR)/compiler/main.o: $(SRC_DIR)/compiler/main.c $(SRC_DIR)/compiler/lexer.h $(SRC_DIR)/compiler/parser.h $(SRC_DIR)/compiler/sema.h $(SRC_DIR)/compiler/codegen.h
$(BUILD_DIR)/compiler/lexer.o: $(SRC_DIR)/compiler/lexer.c $(SRC_DIR)/compiler/lexer.h
$(BUILD_DIR)/compiler/parser.o: $(SRC_DIR)/compiler/parser.c $(SRC_DIR)/compiler/parser.h $(SRC_DIR)/compiler/lexer.h $(SRC_DIR)/compiler/ast.h
$(BUILD_DIR)/compiler/ast.o: $(SRC_DIR)/compiler/ast.c $(SRC_DIR)/compiler/ast.h
$(BUILD_DIR)/compiler/sema.o: $(SRC_DIR)/compiler/sema.c $(SRC_DIR)/compiler/sema.h $(SRC_DIR)/compiler/ast.h
$(BUILD_DIR)/compiler/codegen.o: $(SRC_DIR)/compiler/codegen.c $(SRC_DIR)/compiler/codegen.h $(SRC_DIR)/compiler/ast.h $(SRC_DIR)/common/bytecode.h

$(BUILD_DIR)/vm/main.o: $(SRC_DIR)/vm/main.c $(SRC_DIR)/vm/vm.h
$(BUILD_DIR)/vm/vm.o: $(SRC_DIR)/vm/vm.c $(SRC_DIR)/vm/vm.h $(SRC_DIR)/vm/value.h $(SRC_DIR)/common/bytecode.h
$(BUILD_DIR)/vm/value.o: $(SRC_DIR)/vm/value.c $(SRC_DIR)/vm/value.h $(SRC_DIR)/common/memory.h
$(BUILD_DIR)/vm/agent.o: $(SRC_DIR)/vm/agent.c $(SRC_DIR)/vm/agent.h $(SRC_DIR)/vm/http.h $(SRC_DIR)/vm/value.h $(SRC_DIR)/tui/trace.h
$(BUILD_DIR)/vm/http.o: $(SRC_DIR)/vm/http.c $(SRC_DIR)/vm/http.h $(SRC_DIR)/tui/trace.h
$(BUILD_DIR)/vm/process.o: $(SRC_DIR)/vm/process.c $(SRC_DIR)/vm/process.h $(SRC_DIR)/vm/value.h $(SRC_DIR)/vm/agent.h
$(BUILD_DIR)/vm/scheduler.o: $(SRC_DIR)/vm/scheduler.c $(SRC_DIR)/vm/scheduler.h $(SRC_DIR)/vm/process.h $(SRC_DIR)/vm/vm.h

$(BUILD_DIR)/common/memory.o: $(SRC_DIR)/common/memory.c $(SRC_DIR)/common/memory.h

$(BUILD_DIR)/stdlib/file.o: $(SRC_DIR)/stdlib/file.c $(SRC_DIR)/vm/value.h
$(BUILD_DIR)/stdlib/str.o: $(SRC_DIR)/stdlib/str.c $(SRC_DIR)/vm/value.h
$(BUILD_DIR)/stdlib/json.o: $(SRC_DIR)/stdlib/json.c $(SRC_DIR)/vm/value.h

$(BUILD_DIR)/tui/main.o: $(SRC_DIR)/tui/main.c $(SRC_DIR)/tui/tui.h $(SRC_DIR)/tui/trace.h $(SRC_DIR)/vm/vm.h
$(BUILD_DIR)/tui/tui.o: $(SRC_DIR)/tui/tui.c $(SRC_DIR)/tui/tui.h $(SRC_DIR)/tui/trace.h $(SRC_DIR)/vm/vm.h
$(BUILD_DIR)/tui/trace.o: $(SRC_DIR)/tui/trace.c $(SRC_DIR)/tui/trace.h
$(BUILD_DIR)/tui/repl.o: $(SRC_DIR)/tui/repl.c $(SRC_DIR)/tui/repl.h $(SRC_DIR)/vm/vm.h

# Test targets
test: all
	@echo "Running tests..."
	@for test in tests/*_test.c; do \
		if [ -f "$$test" ]; then \
			$(CC) $(CFLAGS) -o $(BUILD_DIR)/$$(basename $$test .c) $$test $(BUILD_DIR)/compiler/*.o $(BUILD_DIR)/common/*.o -lcurl && \
			$(BUILD_DIR)/$$(basename $$test .c); \
		fi \
	done

# Clean build artifacts
clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# Install (optional)
install: all
	cp $(VEGAC) /usr/local/bin/
	cp $(VEGA) /usr/local/bin/

# Debug build
debug: CFLAGS += -DDEBUG -O0
debug: all

# Release build
release: CFLAGS += -O2 -DNDEBUG
release: clean all

# Address Sanitizer build
asan: CFLAGS += -fsanitize=address -fno-omit-frame-pointer
asan: LDFLAGS += -fsanitize=address
asan: clean all

# Run an example: make run EXAMPLE=hello
EXAMPLE ?= hello
run: all
	@echo "=== Compiling examples/$(EXAMPLE).vega ==="
	@$(BIN_DIR)/vegac examples/$(EXAMPLE).vega -o $(BUILD_DIR)/$(EXAMPLE).vgb
	@echo "=== Running $(EXAMPLE) ==="
	@$(BIN_DIR)/vega $(BUILD_DIR)/$(EXAMPLE).vgb

# Run with verbose compilation
run-verbose: all
	@echo "=== Compiling examples/$(EXAMPLE).vega ==="
	@$(BIN_DIR)/vegac -v examples/$(EXAMPLE).vega -o $(BUILD_DIR)/$(EXAMPLE).vgb
	@echo "=== Running $(EXAMPLE) ==="
	@$(BIN_DIR)/vega $(BUILD_DIR)/$(EXAMPLE).vgb

# Run TUI with an example
tui: all
	@echo "=== Compiling examples/$(EXAMPLE).vega ==="
	@$(BIN_DIR)/vegac examples/$(EXAMPLE).vega -o $(BUILD_DIR)/$(EXAMPLE).vgb
	@echo "=== Launching TUI ==="
	@$(BIN_DIR)/vega tui $(BUILD_DIR)/$(EXAMPLE).vgb

# Cross-compile for Linux using Docker
linux:
	@echo "Building for Linux via Docker..."
	docker build -t vega-linux-builder .
	@mkdir -p bin-linux
	docker create --name vega-extract vega-linux-builder
	docker cp vega-extract:/vega/bin/vega bin-linux/vega
	docker cp vega-extract:/vega/bin/vegac bin-linux/vegac
	docker rm vega-extract
	@echo "Linux binaries in bin-linux/"
