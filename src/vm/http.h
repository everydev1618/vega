#ifndef VEGA_HTTP_H
#define VEGA_HTTP_H

#include "../common/memory.h"
#include <stdbool.h>
#include <pthread.h>

/*
 * Vega HTTP Client
 *
 * Handles communication with the Anthropic API.
 * Supports both synchronous and asynchronous requests.
 */

// ============================================================================
// Token Usage (for budget tracking)
// ============================================================================

typedef struct {
    uint32_t input_tokens;
    uint32_t output_tokens;
    uint32_t cache_read_tokens;
    uint32_t cache_write_tokens;
} HttpTokenUsage;

// ============================================================================
// Response Structure
// ============================================================================

typedef struct {
    int status_code;
    char* body;
    size_t body_len;
    char* error;
    HttpTokenUsage tokens;  // Parsed token usage from response
} HttpResponse;

// ============================================================================
// API
// ============================================================================

// Initialize HTTP client (call once at startup)
bool http_init(void);

// Cleanup HTTP client (call once at shutdown)
void http_cleanup(void);

// Simple HTTP GET request
HttpResponse* http_get(const char* url);

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

// Send tool result back to continue conversation (original)
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

// Send tool result with proper assistant content (for multi-turn tool use)
HttpResponse* anthropic_send_tool_result_v2(
    const char* api_key,
    const char* model,
    const char* system_prompt,
    const char** messages,
    int message_count,
    const char* assistant_content,
    const char* tool_use_id,
    const char* tool_result,
    ToolDefinition* tools,
    int tool_count,
    double temperature
);

// ============================================================================
// Async HTTP Support
// ============================================================================

typedef enum {
    HTTP_ASYNC_PENDING,    // Request in progress
    HTTP_ASYNC_COMPLETE,   // Request finished (check response)
    HTTP_ASYNC_ERROR       // Request failed
} HttpAsyncStatus;

typedef enum {
    HTTP_REQ_MESSAGES,         // anthropic_send_messages
    HTTP_REQ_WITH_TOOLS,       // anthropic_send_with_tools
    HTTP_REQ_TOOL_RESULT_V2    // anthropic_send_tool_result_v2
} HttpRequestType;

typedef struct HttpAsyncRequest {
    // Thread management
    pthread_t thread;
    pthread_mutex_t mutex;
    HttpAsyncStatus status;
    bool thread_started;  // True if pthread_create succeeded

    // Request type and parameters
    HttpRequestType type;
    char* api_key;
    char* model;
    char* system_prompt;
    char** messages;
    int message_count;
    double temperature;

    // For tool requests
    ToolDefinition* tools;
    int tool_count;
    char* assistant_content;  // For tool_result_v2
    char* tool_use_id;
    char* tool_result;

    // Result
    HttpResponse* response;
} HttpAsyncRequest;

// Start an async messages request
HttpAsyncRequest* http_async_send_messages(
    const char* api_key,
    const char* model,
    const char* system_prompt,
    const char** messages,
    int message_count,
    double temperature
);

// Start an async request with tools
HttpAsyncRequest* http_async_send_with_tools(
    const char* api_key,
    const char* model,
    const char* system_prompt,
    const char** messages,
    int message_count,
    ToolDefinition* tools,
    int tool_count,
    double temperature
);

// Start an async tool result request
HttpAsyncRequest* http_async_send_tool_result_v2(
    const char* api_key,
    const char* model,
    const char* system_prompt,
    const char** messages,
    int message_count,
    const char* assistant_content,
    const char* tool_use_id,
    const char* tool_result,
    ToolDefinition* tools,
    int tool_count,
    double temperature
);

// Check if async request is complete (non-blocking)
HttpAsyncStatus http_async_poll(HttpAsyncRequest* req);

// Get result and free request (call after HTTP_ASYNC_COMPLETE)
// Transfers ownership of response to caller
HttpResponse* http_async_get_response(HttpAsyncRequest* req);

// Cancel and free an async request
void http_async_cancel(HttpAsyncRequest* req);

#endif // VEGA_HTTP_H
