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

// ============================================================================
// Agent State
// ============================================================================

// Tool info stored in agent
typedef struct {
    char* name;
    char* description;
    char** param_names;
    char** param_types;
    int param_count;
    uint32_t function_id;       // Index into VM's function table
} AgentTool;

typedef struct VegaAgent {
    VegaObjHeader header;

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

#endif // VEGA_AGENT_H
