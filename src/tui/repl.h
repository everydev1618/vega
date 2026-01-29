#ifndef VEGA_REPL_H
#define VEGA_REPL_H

#include "../vm/vm.h"
#include <stdbool.h>

/*
 * Vega REPL
 *
 * Interactive evaluation of Vega expressions and statements.
 * Used by the TUI for on-the-fly agent interaction.
 */

// ============================================================================
// REPL Session
// ============================================================================

typedef struct {
    VegaVM* vm;

    // Expression evaluation state
    bool in_multiline;          // Currently in multi-line input mode
    char* multiline_buffer;     // Buffer for multi-line input
    size_t multiline_capacity;
    size_t multiline_len;

    // History
    char** history;
    int history_count;
    int history_capacity;

    // Last result
    char* last_result;
    bool last_was_error;

} ReplSession;

// ============================================================================
// API
// ============================================================================

// Create a new REPL session
ReplSession* repl_create(VegaVM* vm);

// Free a REPL session
void repl_free(ReplSession* repl);

// Evaluate input and return result string (caller must free)
// Returns NULL on error, sets error message via repl_get_error()
char* repl_eval(ReplSession* repl, const char* input);

// Check if input is incomplete (needs more lines)
bool repl_needs_more(ReplSession* repl, const char* input);

// Get last error message
const char* repl_get_error(ReplSession* repl);

// Check if last eval was an error
bool repl_was_error(ReplSession* repl);

// Clear error state
void repl_clear_error(ReplSession* repl);

// Add to multi-line buffer
void repl_append_line(ReplSession* repl, const char* line);

// Get current multi-line buffer
const char* repl_get_multiline(ReplSession* repl);

// Clear multi-line buffer
void repl_clear_multiline(ReplSession* repl);

// ============================================================================
// Built-in Commands
// ============================================================================

typedef enum {
    REPL_CMD_NONE,          // Not a built-in command
    REPL_CMD_HELP,          // Show help
    REPL_CMD_QUIT,          // Quit REPL
    REPL_CMD_CLEAR,         // Clear screen/history
    REPL_CMD_HISTORY,       // Show history
    REPL_CMD_LOAD,          // Load file
    REPL_CMD_RUN,           // Run loaded program
    REPL_CMD_AGENTS,        // List agents
    REPL_CMD_VARS,          // List variables
    REPL_CMD_RESET,         // Reset VM state
} ReplCommandType;

// Parse a built-in command, returns command type
ReplCommandType repl_parse_command(const char* input, char** arg);

#endif // VEGA_REPL_H
