#ifndef VEGA_TUI_H
#define VEGA_TUI_H

#include "../vm/vm.h"
#include "trace.h"
#include <ncurses.h>
#include <stdbool.h>

/*
 * Vega TUI - Two Column Layout
 *
 * Layout:
 * +------------------------------------------------------------------+
 * | VEGA                             tokens: X in / Y out  [F1] [F10] |
 * +---------------------------+--------------------------------------+
 * | AGENTS                    | OUTPUT                               |
 * | Coder                     | [Coder] -> "Write a function..."     |
 * |   [thinking]              | [Coder] def is_prime(n):             |
 * |   123 in / 456 out        |     if n < 2:                        |
 * |                           |         return False                 |
 * | Reviewer                  | [Reviewer] -> "Review this code"     |
 * |   [idle]                  | [Reviewer] APPROVED                  |
 * |                           |                                      |
 * +---------------------------+--------------------------------------+
 * | > _                                                              |
 * +------------------------------------------------------------------+
 */

// ============================================================================
// Constants
// ============================================================================

#define TUI_MAX_AGENTS          32    // Max agents to track
#define TUI_INPUT_BUFFER_SIZE   1024  // Input buffer
#define TUI_HISTORY_SIZE        100   // Command history
#define TUI_OUTPUT_BUFFER_SIZE  512   // Max output lines

// ============================================================================
// Agent Info
// ============================================================================

typedef enum {
    AGENT_STATUS_IDLE,
    AGENT_STATUS_THINKING,   // Waiting for API response
    AGENT_STATUS_TOOL_CALL,  // Executing a tool
} AgentStatus;

typedef struct {
    uint32_t agent_id;
    char* name;
    char* model;
    uint64_t total_input_tokens;
    uint64_t total_output_tokens;
    uint64_t total_duration_ms;
    bool active;
    AgentStatus status;
    char* current_tool;      // Tool name if in TOOL_CALL status
} TuiAgentInfo;

// ============================================================================
// Output Line
// ============================================================================

typedef enum {
    OUTPUT_USER_MSG,    // User message sent to agent (->)
    OUTPUT_AGENT_MSG,   // Agent response (<-)
    OUTPUT_PRINT,       // Program print output
    OUTPUT_ERROR,       // Error message
    OUTPUT_TOOL,        // Tool call
    OUTPUT_SYSTEM,      // System message
} OutputType;

typedef struct {
    OutputType type;
    char* agent_name;   // Agent name (NULL for system/print)
    char* text;         // The output text
} OutputLine;

// ============================================================================
// TUI State
// ============================================================================

typedef struct TuiState {
    VegaVM* vm;
    bool running;
    bool needs_refresh;

    // Windows
    WINDOW* header_win;      // Top: header bar
    WINDOW* agents_win;      // Left: agent display
    WINDOW* output_win;      // Right: output display
    WINDOW* input_win;       // Bottom: chat input

    // Agent tracking
    TuiAgentInfo agents[TUI_MAX_AGENTS];
    uint32_t agent_count;

    // Output buffer (ring buffer)
    OutputLine* output_buffer;
    uint32_t output_head;    // Next write position
    uint32_t output_count;   // Current count
    int output_scroll;       // Scroll offset

    // Token totals
    uint64_t total_input_tokens;
    uint64_t total_output_tokens;

    // Input
    char input_buffer[TUI_INPUT_BUFFER_SIZE];
    int input_pos;
    int input_len;

    // Command history
    char** history;
    int history_count;
    int history_capacity;
    int history_pos;

    // Trace subscriber ID
    int trace_subscriber_id;

    // Help mode
    bool show_help;

    // Program execution state
    bool program_running;

    // Error tracking
    char* last_error;           // Most recent error message
    char* last_error_agent;     // Agent that caused the error (if any)
    uint64_t last_error_time;   // Timestamp of last error
    bool error_is_fatal;        // Whether error is fatal vs recoverable

    // Dimensions
    int term_width;
    int term_height;
    int left_col_width;
} TuiState;

// ============================================================================
// TUI API
// ============================================================================

// Initialize TUI
bool tui_init(TuiState* tui, VegaVM* vm);

// Run main event loop
int tui_run(TuiState* tui);

// Cleanup TUI
void tui_cleanup(TuiState* tui);

// Refresh all panels
void tui_refresh(TuiState* tui);

// Add output line
void tui_add_output(TuiState* tui, OutputType type, const char* agent_name, const char* text);

// Track agent
void tui_track_agent(TuiState* tui, uint32_t agent_id, const char* name, const char* model);

// Untrack agent
void tui_untrack_agent(TuiState* tui, uint32_t agent_id);

// Update agent status
void tui_set_agent_status(TuiState* tui, uint32_t agent_id, AgentStatus status, const char* tool_name);

// Update agent token counts
void tui_update_agent_tokens(TuiState* tui, uint32_t agent_id, TokenUsage* tokens, uint64_t duration_ms);

// Get agent name by ID
const char* tui_get_agent_name(TuiState* tui, uint32_t agent_id);

// Process a command
bool tui_process_command(TuiState* tui, const char* command);

// Load and run a program
bool tui_load_program(TuiState* tui, const char* path);

#endif // VEGA_TUI_H
