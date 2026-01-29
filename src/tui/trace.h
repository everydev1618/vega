#ifndef VEGA_TRACE_H
#define VEGA_TRACE_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Vega Tracing System
 *
 * Event-based tracing for debugging and visualization.
 * Supports callbacks for real-time event processing (e.g., TUI updates).
 */

// ============================================================================
// Event Types
// ============================================================================

typedef enum {
    TRACE_AGENT_SPAWN,      // Agent was spawned
    TRACE_AGENT_FREE,       // Agent was freed
    TRACE_MSG_SEND,         // Message sent to agent
    TRACE_MSG_RECV,         // Response received from agent
    TRACE_TOOL_CALL,        // Tool function called
    TRACE_TOOL_RESULT,      // Tool function returned
    TRACE_HTTP_START,       // HTTP request started
    TRACE_HTTP_DONE,        // HTTP request completed
    TRACE_ERROR,            // Error occurred
    TRACE_VM_STEP,          // VM executed instruction (verbose)
    TRACE_PRINT,            // Program print output
} TraceEventType;

// ============================================================================
// Token Usage
// ============================================================================

typedef struct {
    uint32_t input_tokens;
    uint32_t output_tokens;
    uint32_t cache_read_tokens;
    uint32_t cache_write_tokens;
} TokenUsage;

// ============================================================================
// Trace Event
// ============================================================================

typedef struct {
    TraceEventType type;
    uint64_t timestamp_ms;      // Milliseconds since epoch
    uint32_t agent_id;          // Agent ID (0 if N/A)
    char* agent_name;           // Agent name (may be NULL)
    char* data;                 // Event-specific data (JSON or string)
    TokenUsage tokens;          // Token usage (for HTTP events)
    uint64_t duration_ms;       // Duration (for completed operations)
    int status_code;            // HTTP status code (for HTTP_DONE)
    bool is_error;              // Whether this event represents an error
} TraceEvent;

// ============================================================================
// Callback System
// ============================================================================

// Callback function type
typedef void (*TraceCallback)(TraceEvent* event, void* userdata);

// Maximum number of subscribers
#define TRACE_MAX_SUBSCRIBERS 8

// ============================================================================
// API
// ============================================================================

// Initialize the tracing system
void trace_init(void);

// Shutdown the tracing system
void trace_shutdown(void);

// Check if tracing is enabled
bool trace_is_enabled(void);

// Enable/disable tracing globally
void trace_set_enabled(bool enabled);

// Subscribe to trace events
// Returns subscriber ID (0 on failure)
int trace_subscribe(TraceCallback callback, void* userdata);

// Unsubscribe from trace events
void trace_unsubscribe(int subscriber_id);

// Emit a trace event (notifies all subscribers)
void trace_emit(TraceEvent* event);

// ============================================================================
// Convenience Emitters
// ============================================================================

// Emit agent spawn event
void trace_agent_spawn(uint32_t agent_id, const char* name, const char* model);

// Emit agent free event
void trace_agent_free(uint32_t agent_id, const char* name);

// Emit message send event
void trace_msg_send(uint32_t agent_id, const char* agent_name, const char* message);

// Emit message receive event
void trace_msg_recv(uint32_t agent_id, const char* agent_name, const char* response,
                    TokenUsage* tokens, uint64_t duration_ms);

// Emit tool call event
void trace_tool_call(uint32_t agent_id, const char* agent_name,
                     const char* tool_name, const char* input_json);

// Emit tool result event
void trace_tool_result(uint32_t agent_id, const char* agent_name,
                       const char* tool_name, const char* result);

// Emit HTTP start event
void trace_http_start(const char* url, const char* method);

// Emit HTTP done event
void trace_http_done(int status_code, uint64_t duration_ms,
                     TokenUsage* tokens, const char* error);

// Emit error event
void trace_error(uint32_t agent_id, const char* message);

// Emit print output event (for program print statements)
void trace_print(const char* text);

// ============================================================================
// Utility
// ============================================================================

// Get current time in milliseconds
uint64_t trace_get_time_ms(void);

// Get event type name as string
const char* trace_event_type_name(TraceEventType type);

// Free a trace event's allocated memory
void trace_event_free(TraceEvent* event);

// Create a copy of a trace event (caller must free with trace_event_free)
TraceEvent* trace_event_copy(TraceEvent* event);

#endif // VEGA_TRACE_H
