#ifndef VEGA_HTTP_H
#define VEGA_HTTP_H

#include "../common/memory.h"
#include <stdbool.h>

/*
 * Vega HTTP Client
 *
 * Handles communication with the Anthropic API.
 */

// ============================================================================
// Response Structure
// ============================================================================

typedef struct {
    int status_code;
    char* body;
    size_t body_len;
    char* error;
} HttpResponse;

// ============================================================================
// API
// ============================================================================

// Initialize HTTP client (call once at startup)
bool http_init(void);

// Cleanup HTTP client (call once at shutdown)
void http_cleanup(void);

// Send a message to the Anthropic API
// Returns allocated response that must be freed with http_response_free
HttpResponse* anthropic_send_message(
    const char* api_key,
    const char* model,
    const char* system_prompt,
    const char* user_message,
    double temperature
);

// Tool definition for API calls
typedef struct {
    const char* name;
    const char* description;
    const char** param_names;
    const char** param_types;
    int param_count;
} ToolDefinition;

// Send a message with conversation history (for multi-turn)
HttpResponse* anthropic_send_messages(
    const char* api_key,
    const char* model,
    const char* system_prompt,
    const char** messages,     // Alternating user/assistant messages
    int message_count,
    double temperature
);

// Send a message with tools
HttpResponse* anthropic_send_with_tools(
    const char* api_key,
    const char* model,
    const char* system_prompt,
    const char** messages,
    int message_count,
    ToolDefinition* tools,
    int tool_count,
    double temperature
);

// Free response
void http_response_free(HttpResponse* resp);

// Extract text content from API response JSON
// Returns allocated string that must be freed
char* anthropic_extract_text(const char* json_response);

// Check if response contains a tool use
bool anthropic_has_tool_use(const char* json_response);

// Extract tool use details
// Returns tool name (allocated), sets tool_id and input_json
char* anthropic_extract_tool_use(const char* json_response, char** tool_id, char** input_json);

// Send tool result back to continue conversation
HttpResponse* anthropic_send_tool_result(
    const char* api_key,
    const char* model,
    const char* system_prompt,
    const char** messages,
    int message_count,
    const char* tool_use_id,
    const char* tool_result,
    ToolDefinition* tools,
    int tool_count,
    double temperature
);

#endif // VEGA_HTTP_H
