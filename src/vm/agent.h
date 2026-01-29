#ifndef VEGA_AGENT_H
#define VEGA_AGENT_H

#include "value.h"
#include "process.h"
#include "../common/bytecode.h"
#include <stdbool.h>

/*
 * Vega Agent Runtime
 *
 * Manages agent lifecycle and communication with Claude.
 */

// Forward declare VM (defined in vm.h)
struct VegaVM;

// Forward declare async request (defined in http.h)
struct HttpAsyncRequest;

// ============================================================================
// Agent State
// ============================================================================

// Async state machine for agentic loops
typedef enum {
    AGENT_ASYNC_IDLE,           // No pending request
    AGENT_ASYNC_WAITING,        // Waiting for initial/next response
    AGENT_ASYNC_TOOL_PENDING,   // Tool use detected, need to execute and send result
} AgentAsyncState;

// Tool info stored in agent
typedef struct {
    char* name;
    char* description;
    char** param_names;
    char** param_types;
    int param_count;
    uint32_t function_id;       // Index into VM's function table
} AgentTool;

// Context for ongoing tool use loop
typedef struct {
    char* assistant_content;    // Raw content array from API response
    char* tool_use_id;          // Current tool use ID
    char* tool_name;            // Current tool name
    char* tool_input;           // Current tool input JSON
    int iteration;              // Current iteration count
    int max_iterations;         // Max allowed (default 10)
} AgentToolContext;

typedef struct VegaAgent {
    // Note: VegaObjHeader is prepended by vega_obj_alloc, not part of this struct

    // Agent definition
    uint32_t agent_id;          // Index into VM's agent table
    char* name;
    char* model;
    char* system_prompt;
    double temperature;

    // Tools
    AgentTool* tools;
    uint32_t tool_count;

    // Conversation history
    char** messages;            // Alternating user/assistant
    uint32_t message_count;
    uint32_t message_capacity;

    // State
    bool is_valid;              // Set to false when freed

    // Process (for supervised agents)
    VegaProcess* process;       // NULL if not supervised

    // Async request tracking
    struct HttpAsyncRequest* pending_request;  // NULL when idle
    AgentAsyncState async_state;               // Current state in async loop
    AgentToolContext tool_ctx;                 // Context for tool use loop
} VegaAgent;

// ============================================================================
// API
// ============================================================================

// Spawn a new agent instance
VegaAgent* agent_spawn(struct VegaVM* vm, uint32_t agent_def_id);

// Spawn a supervised agent (creates both agent and process)
VegaAgent* agent_spawn_supervised(struct VegaVM* vm, uint32_t agent_def_id,
                                   SupervisionConfig* config);

// Free an agent
void agent_free(VegaAgent* agent);

// Send a message to the agent and get response
VegaString* agent_send_message(struct VegaVM* vm, VegaAgent* agent, const char* message);

// Check if agent handle is valid
bool agent_is_valid(VegaAgent* agent);

// Get agent name
const char* agent_get_name(VegaAgent* agent);

// Clear conversation history
void agent_clear_history(VegaAgent* agent);

// ============================================================================
// Async Message API
// ============================================================================

// Start an async message send (non-blocking)
// Returns true if request was started, false on error
bool agent_start_message_async(struct VegaVM* vm, VegaAgent* agent, const char* message);

// Poll for async message completion (non-blocking)
// Returns: 0 = pending, 1 = complete, -1 = error
int agent_poll_message(VegaAgent* agent);

// Get the result of a completed async message
// Returns NULL if not complete or error
// Caller must not free the result - it's owned by the agent
VegaString* agent_get_message_result(struct VegaVM* vm, VegaAgent* agent);

// Check if agent has a pending async request
bool agent_has_pending_request(VegaAgent* agent);

// Cancel any pending async request
void agent_cancel_pending(VegaAgent* agent);

#endif // VEGA_AGENT_H
