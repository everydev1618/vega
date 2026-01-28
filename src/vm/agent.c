#include "agent.h"
#include "vm.h"
#include "http.h"
#include "scheduler.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// JSON Parsing Helpers
// ============================================================================

// Extract a string value from JSON by key
static char* json_get_string(const char* json, const char* key) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\":", key);

    const char* pos = strstr(json, search);
    if (!pos) return NULL;

    pos += strlen(search);
    while (*pos == ' ' || *pos == '\t' || *pos == '\n') pos++;

    if (*pos != '"') return NULL;
    pos++;

    const char* end = pos;
    while (*end && *end != '"') {
        if (*end == '\\' && *(end + 1)) end += 2;
        else end++;
    }

    return strndup(pos, end - pos);
}

// ============================================================================
// Agent Lifecycle
// ============================================================================

VegaAgent* agent_spawn(VegaVM* vm, uint32_t agent_def_id) {
    AgentDef* def = vm_get_agent(vm, agent_def_id);
    if (!def) {
        fprintf(stderr, "Error: Invalid agent definition id: %u\n", agent_def_id);
        return NULL;
    }

    VegaAgent* agent = vega_obj_alloc(sizeof(VegaAgent) - sizeof(VegaObjHeader), OBJ_AGENT);
    if (!agent) {
        fprintf(stderr, "Error: Failed to allocate agent\n");
        return NULL;
    }

    agent->agent_id = agent_def_id;

    // Get name from constant pool
    uint32_t len;
    const char* name = vm_read_string(vm, def->name_idx, &len);
    agent->name = name ? strndup(name, len) : strdup("unnamed");

    // Get model
    const char* model = vm_read_string(vm, def->model_idx, &len);
    agent->model = model ? strndup(model, len) : strdup("claude-sonnet-4-20250514");

    // Get system prompt
    const char* system = vm_read_string(vm, def->system_idx, &len);
    agent->system_prompt = system ? strndup(system, len) : NULL;

    // Temperature
    agent->temperature = def->temperature_x100 / 100.0;

    // Load tools - look for functions named "AgentName$toolname"
    agent->tools = NULL;
    agent->tool_count = 0;

    if (def->tool_count > 0) {
        agent->tools = calloc(def->tool_count, sizeof(AgentTool));

        char prefix[256];
        snprintf(prefix, sizeof(prefix), "%s$", agent->name);
        size_t prefix_len = strlen(prefix);

        // Find matching functions
        for (uint32_t i = 0; i < vm->func_count && agent->tool_count < def->tool_count; i++) {
            uint32_t fn_len;
            const char* fn_name = vm_read_string(vm, vm->functions[i].name_idx, &fn_len);

            if (fn_name && fn_len > prefix_len && strncmp(fn_name, prefix, prefix_len) == 0) {
                AgentTool* tool = &agent->tools[agent->tool_count++];
                tool->name = strndup(fn_name + prefix_len, fn_len - prefix_len);
                tool->description = NULL;  // TODO: store descriptions in bytecode
                tool->function_id = i;
                tool->param_count = vm->functions[i].param_count;

                // Create param names (we don't have them stored, use generic names)
                if (tool->param_count > 0) {
                    tool->param_names = malloc(tool->param_count * sizeof(char*));
                    tool->param_types = malloc(tool->param_count * sizeof(char*));
                    for (int p = 0; p < tool->param_count; p++) {
                        char pname[32];
                        snprintf(pname, sizeof(pname), "arg%d", p);
                        tool->param_names[p] = strdup(pname);
                        tool->param_types[p] = strdup("str");  // Default to string
                    }
                } else {
                    tool->param_names = NULL;
                    tool->param_types = NULL;
                }
            }
        }
    }

    // Initialize conversation history
    agent->messages = NULL;
    agent->message_count = 0;
    agent->message_capacity = 0;

    agent->is_valid = true;
    agent->process = NULL;

    return agent;
}

// Internal cleanup function - called by value_release when refcount hits 0
static void agent_cleanup(VegaAgent* agent) {
    if (!agent) return;

    agent->is_valid = false;

    free(agent->name);
    free(agent->model);
    free(agent->system_prompt);

    // Free tools
    for (uint32_t i = 0; i < agent->tool_count; i++) {
        free(agent->tools[i].name);
        free(agent->tools[i].description);
        for (int p = 0; p < agent->tools[i].param_count; p++) {
            free(agent->tools[i].param_names[p]);
            free(agent->tools[i].param_types[p]);
        }
        free(agent->tools[i].param_names);
        free(agent->tools[i].param_types);
    }
    free(agent->tools);

    for (uint32_t i = 0; i < agent->message_count; i++) {
        free(agent->messages[i]);
    }
    free(agent->messages);
}

void agent_free(VegaAgent* agent) {
    if (!agent) return;
    agent_cleanup(agent);
    vega_obj_release(agent);
}

VegaAgent* agent_spawn_supervised(VegaVM* vm, uint32_t agent_def_id,
                                   SupervisionConfig* config) {
    // Create the agent
    VegaAgent* agent = agent_spawn(vm, agent_def_id);
    if (!agent) return NULL;

    // Create a process for the agent
    VegaProcess* proc = process_create(vm, 0);  // 0 = no parent (will be set later)
    if (!proc) {
        agent_free(agent);
        return NULL;
    }

    // Apply supervision config
    if (config) {
        proc->supervision = *config;
    }

    // Link agent and process
    agent->process = proc;
    proc->agent = agent;
    proc->agent_def_id = agent_def_id;

    // Add to VM's process table
    if (vm->process_count < MAX_PROCESSES) {
        vm->processes[vm->process_count++] = proc;
        vm->scheduler.processes_spawned++;
    } else {
        fprintf(stderr, "Warning: Max processes reached, agent not supervised\n");
        process_free(proc);
        agent->process = NULL;
    }

    return agent;
}

// ============================================================================
// Tool Execution
// ============================================================================

// Execute a tool function and return the result as a string
static char* execute_tool(VegaVM* vm, VegaAgent* agent, const char* tool_name, const char* input_json) {
    // Find the tool
    AgentTool* tool = NULL;
    for (uint32_t i = 0; i < agent->tool_count; i++) {
        if (strcmp(agent->tools[i].name, tool_name) == 0) {
            tool = &agent->tools[i];
            break;
        }
    }

    if (!tool) {
        char err[256];
        snprintf(err, sizeof(err), "Error: Unknown tool '%s'", tool_name);
        return strdup(err);
    }

    // Get the function
    if (tool->function_id >= vm->func_count) {
        return strdup("Error: Invalid tool function");
    }

    FunctionDef* fn = &vm->functions[tool->function_id];

    // Save VM state
    uint32_t saved_ip = vm->ip;
    uint32_t saved_sp = vm->sp;
    uint32_t saved_frame_count = vm->frame_count;

    // Push arguments from JSON
    // Parse input_json to extract arguments
    for (int p = 0; p < tool->param_count; p++) {
        char* arg_val = json_get_string(input_json, tool->param_names[p]);
        if (arg_val) {
            VegaString* str = vega_string_from_cstr(arg_val);
            vm_push(vm, value_string(str));
            free(arg_val);
        } else {
            // Push empty string for missing args
            vm_push(vm, value_string(vega_string_from_cstr("")));
        }
    }

    // Set up call frame
    if (vm->frame_count < VM_FRAMES_MAX) {
        CallFrame* frame = &vm->frames[vm->frame_count++];
        frame->function_id = tool->function_id;
        frame->ip = vm->ip;
        frame->bp = vm->sp - tool->param_count;

        // Reserve space for locals
        while (vm->sp < frame->bp + fn->local_count) {
            vm_push(vm, value_null());
        }

        // Execute from function start
        vm->ip = fn->code_offset;

        // Run until return
        while (vm->running && vm->frame_count > saved_frame_count) {
            vm_step(vm);
        }
    }

    // Get result
    char* result;
    if (vm->sp > saved_sp) {
        Value result_val = vm_pop(vm);
        VegaString* result_str = value_to_string(result_val);
        result = strdup(result_str->data);
        vega_obj_release(result_str);
        value_release(result_val);
    } else {
        result = strdup("");
    }

    // Restore VM state
    vm->ip = saved_ip;
    while (vm->sp > saved_sp) {
        value_release(vm_pop(vm));
    }
    vm->frame_count = saved_frame_count;

    return result;
}

// ============================================================================
// Message Passing
// ============================================================================

static void add_message(VegaAgent* agent, const char* message) {
    if (agent->message_count >= agent->message_capacity) {
        agent->message_capacity = agent->message_capacity == 0 ? 8 : agent->message_capacity * 2;
        agent->messages = realloc(agent->messages, agent->message_capacity * sizeof(char*));
    }
    agent->messages[agent->message_count++] = strdup(message);
}

VegaString* agent_send_message(VegaVM* vm, VegaAgent* agent, const char* message) {
    if (!agent || !agent->is_valid) {
        return vega_string_from_cstr("Error: Invalid agent");
    }

    if (!vm->api_key) {
        fprintf(stderr, "Error: ANTHROPIC_API_KEY not set\n");
        return vega_string_from_cstr("Error: ANTHROPIC_API_KEY not set");
    }

    // Add user message to history
    add_message(agent, message);

    // Build tool definitions if agent has tools
    ToolDefinition* tool_defs = NULL;
    if (agent->tool_count > 0) {
        tool_defs = calloc(agent->tool_count, sizeof(ToolDefinition));
        for (uint32_t i = 0; i < agent->tool_count; i++) {
            tool_defs[i].name = agent->tools[i].name;
            tool_defs[i].description = agent->tools[i].description ?
                agent->tools[i].description : "A tool function";
            tool_defs[i].param_names = agent->tools[i].param_names;
            tool_defs[i].param_types = agent->tools[i].param_types;
            tool_defs[i].param_count = agent->tools[i].param_count;
        }
    }

    // Tool use loop
    int max_iterations = 10;  // Prevent infinite loops
    for (int iter = 0; iter < max_iterations; iter++) {
        HttpResponse* resp;

        if (tool_defs && agent->tool_count > 0) {
            resp = anthropic_send_with_tools(
                vm->api_key,
                agent->model,
                agent->system_prompt,
                (const char**)agent->messages,
                (int)agent->message_count,
                tool_defs,
                (int)agent->tool_count,
                agent->temperature
            );
        } else {
            resp = anthropic_send_messages(
                vm->api_key,
                agent->model,
                agent->system_prompt,
                (const char**)agent->messages,
                (int)agent->message_count,
                agent->temperature
            );
        }

        if (!resp) {
            free(tool_defs);
            return vega_string_from_cstr("Error: Failed to send message");
        }

        if (resp->error) {
            char error_buf[512];
            snprintf(error_buf, sizeof(error_buf), "Error: %s", resp->error);
            VegaString* result = vega_string_from_cstr(error_buf);
            http_response_free(resp);
            free(tool_defs);
            return result;
        }

        if (resp->status_code != 200) {
            char error_buf[512];
            snprintf(error_buf, sizeof(error_buf),
                    "Error: API returned status %d", resp->status_code);
            VegaString* result = vega_string_from_cstr(error_buf);
            http_response_free(resp);
            free(tool_defs);
            return result;
        }

        // Check for tool use
        if (resp->body && anthropic_has_tool_use(resp->body)) {
            char* tool_id = NULL;
            char* tool_input = NULL;

            char* tool_name = anthropic_extract_tool_use(resp->body, &tool_id, &tool_input);
            if (tool_name) {
                // Execute the tool
                char* tool_result = execute_tool(vm, agent, tool_name, tool_input);

                // Send tool result back
                http_response_free(resp);
                resp = anthropic_send_tool_result(
                    vm->api_key,
                    agent->model,
                    agent->system_prompt,
                    (const char**)agent->messages,
                    (int)agent->message_count,
                    tool_id,
                    tool_result,
                    tool_defs,
                    (int)agent->tool_count,
                    agent->temperature
                );

                free(tool_id);
                free(tool_name);
                free(tool_input);
                free(tool_result);

                if (!resp || resp->status_code != 200) {
                    free(tool_defs);
                    if (resp) http_response_free(resp);
                    return vega_string_from_cstr("Error: Tool result submission failed");
                }

                // Check if we got a final response or another tool call
                if (resp->body && !anthropic_has_tool_use(resp->body)) {
                    // Got final response
                    char* text = anthropic_extract_text(resp->body);
                    if (text) {
                        add_message(agent, text);
                        VegaString* result = vega_string_from_cstr(text);
                        free(text);
                        http_response_free(resp);
                        free(tool_defs);
                        return result;
                    }
                }
                // Continue loop for another tool call
                http_response_free(resp);
                continue;
            }
        }

        // No tool use - extract text response
        char* text = anthropic_extract_text(resp->body);
        http_response_free(resp);
        free(tool_defs);

        if (text) {
            add_message(agent, text);
            VegaString* result = vega_string_from_cstr(text);
            free(text);
            return result;
        }

        return vega_string_from_cstr("Error: No response from API");
    }

    free(tool_defs);
    return vega_string_from_cstr("Error: Max tool iterations exceeded");
}

// ============================================================================
// Utility Functions
// ============================================================================

bool agent_is_valid(VegaAgent* agent) {
    return agent && agent->is_valid;
}

const char* agent_get_name(VegaAgent* agent) {
    return agent ? agent->name : NULL;
}

void agent_clear_history(VegaAgent* agent) {
    if (!agent) return;

    for (uint32_t i = 0; i < agent->message_count; i++) {
        free(agent->messages[i]);
    }
    agent->message_count = 0;
}
