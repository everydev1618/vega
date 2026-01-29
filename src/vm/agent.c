#include "agent.h"
#include "vm.h"
#include "http.h"
#include "scheduler.h"
#include "../tui/trace.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <unistd.h>  // for usleep

// ============================================================================
// Error Classification
// ============================================================================

typedef enum {
    ERROR_NONE,
    ERROR_RETRIABLE,      // Rate limit, timeout, temporary failure
    ERROR_FATAL,          // Auth error, bad request, etc.
} ErrorType;

// Classify an HTTP status code and response body
static ErrorType classify_error(int status_code, const char* body) {
    if (status_code == 200) {
        return ERROR_NONE;
    }

    // Rate limit - retriable
    if (status_code == 429) {
        return ERROR_RETRIABLE;
    }

    // Server errors - retriable
    if (status_code >= 500 && status_code < 600) {
        return ERROR_RETRIABLE;
    }

    // Timeout (status 0 usually means network error)
    if (status_code == 0) {
        return ERROR_RETRIABLE;
    }

    // Check for overloaded error in body
    if (body && strstr(body, "overloaded")) {
        return ERROR_RETRIABLE;
    }

    // All other errors are fatal (4xx except 429)
    return ERROR_FATAL;
}

// Helper to get current time in milliseconds
static uint64_t get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// ============================================================================
// JSON Parsing Helpers
// ============================================================================

// Extract any JSON value by key (works with strings, numbers, bools)
// Returns the raw value as a string, and sets *is_string if it was quoted
static char* json_get_value(const char* json, const char* key, bool* is_string) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\":", key);

    const char* pos = strstr(json, search);
    if (!pos) return NULL;

    pos += strlen(search);
    while (*pos == ' ' || *pos == '\t' || *pos == '\n') pos++;

    if (*pos == '"') {
        // Quoted string
        if (is_string) *is_string = true;
        pos++;
        const char* end = pos;
        while (*end && *end != '"') {
            if (*end == '\\' && *(end + 1)) end += 2;
            else end++;
        }
        return strndup(pos, end - pos);
    } else {
        // Unquoted value (number, bool, null)
        if (is_string) *is_string = false;
        const char* end = pos;
        while (*end && *end != ',' && *end != '}' && *end != ']' &&
               *end != ' ' && *end != '\t' && *end != '\n') {
            end++;
        }
        return strndup(pos, end - pos);
    }
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

    VegaAgent* agent = vega_obj_alloc(sizeof(VegaAgent), OBJ_AGENT);
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
    if (!agent->model) {
        free(agent->name);
        free(agent);
        return NULL;
    }

    // Get system prompt
    const char* system = vm_read_string(vm, def->system_idx, &len);
    if (system) {
        agent->system_prompt = strndup(system, len);
        if (!agent->system_prompt) {
            free(agent->model);
            free(agent->name);
            free(agent);
            return NULL;
        }
    } else {
        agent->system_prompt = NULL;
    }

    // Temperature
    agent->temperature = def->temperature_x100 / 100.0;

    // Load tools - look for functions named "AgentName$toolname"
    agent->tools = NULL;
    agent->tool_count = 0;

    if (def->tool_count > 0) {
        agent->tools = calloc(def->tool_count, sizeof(AgentTool));
        if (!agent->tools) {
            free(agent->system_prompt);
            free(agent->model);
            free(agent->name);
            free(agent);
            return NULL;
        }

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
                if (!tool->name) {
                    // Allocation failed - clean up partially initialized agent
                    agent->tool_count--;  // Don't count this failed tool
                    agent_free(agent);
                    return NULL;
                }
                tool->description = NULL;  // TODO: store descriptions in bytecode
                tool->function_id = i;
                tool->param_count = vm->functions[i].param_count;

                // Look for param info in constant pool: "AgentName$toolname$params" -> "name:type,..."
                if (tool->param_count > 0) {
                    tool->param_names = calloc(tool->param_count, sizeof(char*));
                    tool->param_types = calloc(tool->param_count, sizeof(char*));
                    if (!tool->param_names || !tool->param_types) {
                        // Allocation failed - set param_count to 0 and continue
                        // (tool will still work but API won't know param names)
                        free(tool->param_names);
                        free(tool->param_types);
                        tool->param_names = NULL;
                        tool->param_types = NULL;
                        tool->param_count = 0;
                    } else {
                        // Build the params key
                        char params_key[280];
                        snprintf(params_key, sizeof(params_key), "%.*s$params", (int)fn_len, fn_name);

                        uint32_t params_len;
                        const char* params_str = vm_find_string_after_key(vm, params_key, &params_len);

                        if (params_str && params_len > 0) {
                            // Parse "name1:type1,name2:type2"
                            char* params_copy = strndup(params_str, params_len);
                            if (params_copy) {
                                char* saveptr = NULL;
                                char* token = strtok_r(params_copy, ",", &saveptr);
                                int p = 0;

                                while (token && p < tool->param_count) {
                                    char* colon = strchr(token, ':');
                                    if (colon) {
                                        *colon = '\0';
                                        tool->param_names[p] = strdup(token);
                                        tool->param_types[p] = strdup(colon + 1);
                                    } else {
                                        tool->param_names[p] = strdup(token);
                                        tool->param_types[p] = strdup("str");
                                    }
                                    // Note: strdup failures leave NULL which is handled at use
                                    token = strtok_r(NULL, ",", &saveptr);
                                    p++;
                                }
                                free(params_copy);

                                // Fill remaining with defaults if needed
                                while (p < tool->param_count) {
                                    char pname[32];
                                    snprintf(pname, sizeof(pname), "arg%d", p);
                                    tool->param_names[p] = strdup(pname);
                                    tool->param_types[p] = strdup("str");
                                    p++;
                                }
                            }
                        } else {
                            // Fall back to generic names
                            for (int p = 0; p < tool->param_count; p++) {
                                char pname[32];
                                snprintf(pname, sizeof(pname), "arg%d", p);
                                tool->param_names[p] = strdup(pname);
                                tool->param_types[p] = strdup("str");
                            }
                        }
                    }  // end of if (param_names && param_types allocation succeeded)
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
    agent->pending_request = NULL;
    agent->async_state = AGENT_ASYNC_IDLE;
    memset(&agent->tool_ctx, 0, sizeof(AgentToolContext));
    agent->tool_ctx.max_iterations = 10;

    // Emit trace event
    trace_agent_spawn(agent_def_id, agent->name, agent->model);

    return agent;
}

// Helper to clear tool context
static void clear_tool_context(AgentToolContext* ctx) {
    free(ctx->assistant_content);
    free(ctx->tool_use_id);
    free(ctx->tool_name);
    free(ctx->tool_input);
    ctx->assistant_content = NULL;
    ctx->tool_use_id = NULL;
    ctx->tool_name = NULL;
    ctx->tool_input = NULL;
    ctx->iteration = 0;
}

// Internal cleanup function - called by value_release when refcount hits 0
static void agent_cleanup(VegaAgent* agent) {
    if (!agent) return;

    // Cancel any pending async request
    if (agent->pending_request) {
        http_async_cancel(agent->pending_request);
        agent->pending_request = NULL;
    }

    // Clean up tool context
    clear_tool_context(&agent->tool_ctx);

    // Emit trace event before cleanup
    trace_agent_free(agent->agent_id, agent->name);

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
    // Parse input_json to extract arguments, inferring types from JSON values
    for (int p = 0; p < tool->param_count; p++) {
        bool is_string = false;
        char* arg_val = json_get_value(input_json, tool->param_names[p], &is_string);

        if (!arg_val) {
            // No value found, push null
            vm_push(vm, value_null());
            continue;
        }

        if (is_string) {
            // Quoted string in JSON -> push as string
            VegaString* str = vega_string_from_cstr(arg_val);
            vm_push(vm, value_string(str));
        } else if (strcmp(arg_val, "true") == 0) {
            vm_push(vm, value_bool(true));
        } else if (strcmp(arg_val, "false") == 0) {
            vm_push(vm, value_bool(false));
        } else if (strcmp(arg_val, "null") == 0) {
            vm_push(vm, value_null());
        } else if (strchr(arg_val, '.') != NULL) {
            // Has decimal point -> float
            double float_val = strtod(arg_val, NULL);
            vm_push(vm, value_float(float_val));
        } else {
            // Assume integer
            int64_t int_val = strtoll(arg_val, NULL, 10);
            vm_push(vm, value_int(int_val));
        }
        free(arg_val);
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

// Returns false on allocation failure
static bool add_message(VegaAgent* agent, const char* message) {
    if (agent->message_count >= agent->message_capacity) {
        uint32_t new_capacity = agent->message_capacity == 0 ? 8 : agent->message_capacity * 2;
        char** new_messages = realloc(agent->messages, new_capacity * sizeof(char*));
        if (!new_messages) {
            return false;  // Allocation failed, original messages preserved
        }
        agent->messages = new_messages;
        agent->message_capacity = new_capacity;
    }
    char* msg_copy = strdup(message);
    if (!msg_copy) {
        return false;  // strdup failed
    }
    agent->messages[agent->message_count++] = msg_copy;
    return true;
}

VegaString* agent_send_message(VegaVM* vm, VegaAgent* agent, const char* message) {
    if (!agent || !agent->is_valid) {
        trace_error(0, "Invalid agent");
        return vega_string_from_cstr("Error: Invalid agent");
    }

    if (!vm->api_key) {
        trace_error(agent->agent_id, "ANTHROPIC_API_KEY not set");
        return vega_string_from_cstr("Error: ANTHROPIC_API_KEY not set");
    }

    // Emit trace for message send
    trace_msg_send(agent->agent_id, agent->name, message);
    uint64_t start_time = get_time_ms();

    // Add user message to history
    if (!add_message(agent, message)) {
        trace_error(agent->agent_id, "Out of memory adding message to history");
        return vega_string_from_cstr("Error: Out of memory");
    }

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

        fflush(stderr);

        if (!resp) {
            trace_error(agent->agent_id, "Failed to send HTTP request");
            free(tool_defs);
            return vega_string_from_cstr("Error: Failed to send message");
        }

        // Track token usage for budget
        vm_add_token_usage(vm, resp->tokens.input_tokens, resp->tokens.output_tokens);

        // Check budget limits
        if (vm_budget_exceeded(vm)) {
            char error_buf[256];
            snprintf(error_buf, sizeof(error_buf),
                    "Error: Budget exceeded (in: %llu, out: %llu, cost: $%.4f)",
                    (unsigned long long)vm->budget_used_input_tokens,
                    (unsigned long long)vm->budget_used_output_tokens,
                    vm->budget_used_cost_usd);
            VegaString* result = vega_string_from_cstr(error_buf);
            http_response_free(resp);
            free(tool_defs);
            trace_error(agent->agent_id, "Budget exceeded");
            return result;
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
                // Emit trace for tool call
                trace_tool_call(agent->agent_id, agent->name, tool_name, tool_input);

                // Execute the tool
                char* tool_result = execute_tool(vm, agent, tool_name, tool_input);

                // Emit trace for tool result
                trace_tool_result(agent->agent_id, agent->name, tool_name, tool_result);

                // Extract content array from assistant response for proper API formatting
                char* assistant_content = NULL;
                const char* content_start = strstr(resp->body, "\"content\":");
                if (content_start) {
                    content_start += 10;
                    while (*content_start == ' ' || *content_start == '\t' || *content_start == '\n') content_start++;
                    if (*content_start == '[') {
                        const char* content_end = content_start + 1;
                        int depth = 1;
                        while (*content_end && depth > 0) {
                            if (*content_end == '[') depth++;
                            else if (*content_end == ']') depth--;
                            else if (*content_end == '"') {
                                content_end++;
                                while (*content_end && *content_end != '"') {
                                    if (*content_end == '\\' && *(content_end + 1)) content_end++;
                                    content_end++;
                                }
                            }
                            if (*content_end) content_end++;
                        }
                        assistant_content = strndup(content_start, content_end - content_start);
                    }
                }

                // Send tool result back with assistant content
                http_response_free(resp);
                resp = anthropic_send_tool_result_v2(
                    vm->api_key,
                    agent->model,
                    agent->system_prompt,
                    (const char**)agent->messages,
                    (int)agent->message_count,
                    assistant_content,
                    tool_id,
                    tool_result,
                    tool_defs,
                    (int)agent->tool_count,
                    agent->temperature
                );
                free(assistant_content);

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
                        uint64_t duration = get_time_ms() - start_time;
                        TokenUsage tokens = {0};  // TODO: parse from response
                        trace_msg_recv(agent->agent_id, agent->name, text, &tokens, duration);

                        // Best effort - history add failure is non-fatal
                        (void)add_message(agent, text);
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
            uint64_t duration = get_time_ms() - start_time;
            TokenUsage tokens = {0};  // TODO: parse from response
            trace_msg_recv(agent->agent_id, agent->name, text, &tokens, duration);

            // Best effort - history add failure is non-fatal
            (void)add_message(agent, text);
            VegaString* result = vega_string_from_cstr(text);
            free(text);
            return result;
        }

        trace_error(agent->agent_id, "No response from API");
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

// ============================================================================
// Async Message API
// ============================================================================

bool agent_start_message_async(VegaVM* vm, VegaAgent* agent, const char* message) {
    if (!agent || !agent->is_valid) {
        trace_error(0, "Invalid agent");
        return false;
    }

    if (!vm->api_key) {
        trace_error(agent->agent_id, "ANTHROPIC_API_KEY not set");
        return false;
    }

    // Can't start a new request if one is pending
    if (agent->pending_request) {
        trace_error(agent->agent_id, "Agent already has pending request");
        return false;
    }

    // Emit trace for message send
    trace_msg_send(agent->agent_id, agent->name, message);

    // Add user message to history
    if (!add_message(agent, message)) {
        trace_error(agent->agent_id, "Out of memory adding message to history");
        return false;
    }

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

    // Start async request
    HttpAsyncRequest* req;
    if (tool_defs && agent->tool_count > 0) {
        req = http_async_send_with_tools(
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
        req = http_async_send_messages(
            vm->api_key,
            agent->model,
            agent->system_prompt,
            (const char**)agent->messages,
            (int)agent->message_count,
            agent->temperature
        );
    }

    free(tool_defs);

    if (!req) {
        trace_error(agent->agent_id, "Failed to start async request");
        return false;
    }

    agent->pending_request = req;
    agent->async_state = AGENT_ASYNC_WAITING;
    agent->tool_ctx.iteration = 0;
    return true;
}

int agent_poll_message(VegaAgent* agent) {
    if (!agent) return -1;

    // Check async state
    if (agent->async_state == AGENT_ASYNC_IDLE) {
        return -1;  // No active request
    }

    if (!agent->pending_request) {
        return -1;  // Error - bad state
    }

    HttpAsyncStatus status = http_async_poll(agent->pending_request);
    switch (status) {
        case HTTP_ASYNC_PENDING:
            return 0;  // Still pending
        case HTTP_ASYNC_COMPLETE:
            return 1;  // HTTP complete - caller should get result
        case HTTP_ASYNC_ERROR:
        default:
            return -1; // Error
    }
}

// Helper to extract assistant content array from response body
static char* extract_assistant_content(const char* body) {
    const char* content_start = strstr(body, "\"content\":");
    if (!content_start) return NULL;

    content_start += 10;
    while (*content_start == ' ' || *content_start == '\t' || *content_start == '\n') content_start++;

    if (*content_start != '[') return NULL;

    const char* content_end = content_start + 1;
    int depth = 1;
    while (*content_end && depth > 0) {
        if (*content_end == '[') depth++;
        else if (*content_end == ']') depth--;
        else if (*content_end == '"') {
            content_end++;
            while (*content_end && *content_end != '"') {
                if (*content_end == '\\' && *(content_end + 1)) content_end++;
                content_end++;
            }
        }
        if (*content_end) content_end++;
    }
    return strndup(content_start, content_end - content_start);
}

// Helper to build tool definitions array
static ToolDefinition* build_tool_defs(VegaAgent* agent) {
    if (agent->tool_count == 0) return NULL;

    ToolDefinition* tool_defs = calloc(agent->tool_count, sizeof(ToolDefinition));
    for (uint32_t i = 0; i < agent->tool_count; i++) {
        tool_defs[i].name = agent->tools[i].name;
        tool_defs[i].description = agent->tools[i].description ?
            agent->tools[i].description : "A tool function";
        tool_defs[i].param_names = agent->tools[i].param_names;
        tool_defs[i].param_types = agent->tools[i].param_types;
        tool_defs[i].param_count = agent->tools[i].param_count;
    }
    return tool_defs;
}

VegaString* agent_get_message_result(VegaVM* vm, VegaAgent* agent) {
    if (!agent || !agent->pending_request) {
        agent->async_state = AGENT_ASYNC_IDLE;
        return vega_string_from_cstr("Error: No pending request");
    }

    // Get the response
    HttpResponse* resp = http_async_get_response(agent->pending_request);
    agent->pending_request = NULL;  // Request is consumed

    if (!resp) {
        agent->async_state = AGENT_ASYNC_IDLE;
        clear_tool_context(&agent->tool_ctx);
        trace_error(agent->agent_id, "Failed to get async response");
        return vega_string_from_cstr("Error: Failed to get response");
    }

    // Track token usage for budget
    vm_add_token_usage(vm, resp->tokens.input_tokens, resp->tokens.output_tokens);

    // Check budget limits
    if (vm_budget_exceeded(vm)) {
        char error_buf[256];
        snprintf(error_buf, sizeof(error_buf),
                "Error: Budget exceeded (in: %llu, out: %llu, cost: $%.4f)",
                (unsigned long long)vm->budget_used_input_tokens,
                (unsigned long long)vm->budget_used_output_tokens,
                vm->budget_used_cost_usd);
        VegaString* result = vega_string_from_cstr(error_buf);
        http_response_free(resp);
        agent->async_state = AGENT_ASYNC_IDLE;
        clear_tool_context(&agent->tool_ctx);
        trace_error(agent->agent_id, "Budget exceeded");
        return result;
    }

    // Check for errors and handle retry with supervision
    ErrorType err_type = classify_error(resp->status_code, resp->body);

    if (resp->error || err_type != ERROR_NONE) {
        // Record failure for circuit breaker
        if (agent->process) {
            process_record_failure(agent->process);
        }

        // Check if retriable and supervised
        if (err_type == ERROR_RETRIABLE && agent->process) {
            // Check circuit breaker
            if (!process_circuit_allows(agent->process)) {
                char error_buf[256];
                snprintf(error_buf, sizeof(error_buf),
                        "Error: Circuit breaker open - too many failures");
                VegaString* result = vega_string_from_cstr(error_buf);
                http_response_free(resp);
                agent->async_state = AGENT_ASYNC_IDLE;
                clear_tool_context(&agent->tool_ctx);
                trace_error(agent->agent_id, "Circuit breaker open");
                return result;
            }

            // Schedule retry with backoff
            int32_t delay = process_schedule_retry(agent->process);
            if (delay >= 0) {
                // Log retry attempt
                fprintf(stderr, "[supervision] Agent %s: retriable error (status %d), "
                        "retrying in %d ms (attempt %u/%u)\n",
                        agent->name, resp->status_code, delay,
                        agent->process->supervision.restart_count + 1,
                        agent->process->supervision.max_restarts);

                // Wait for backoff delay (this is blocking but necessary for retry)
                if (delay > 0) {
                    usleep(delay * 1000);
                }

                // Increment retry count
                agent->process->supervision.restart_count++;

                // Restart the request - rebuild and send again
                ToolDefinition* tool_defs = build_tool_defs(agent);
                HttpAsyncRequest* retry_req;

                if (tool_defs && agent->tool_count > 0) {
                    retry_req = http_async_send_with_tools(
                        vm->api_key, agent->model, agent->system_prompt,
                        (const char**)agent->messages, (int)agent->message_count,
                        tool_defs, (int)agent->tool_count, agent->temperature
                    );
                } else {
                    retry_req = http_async_send_messages(
                        vm->api_key, agent->model, agent->system_prompt,
                        (const char**)agent->messages, (int)agent->message_count,
                        agent->temperature
                    );
                }
                free(tool_defs);
                http_response_free(resp);

                if (retry_req) {
                    agent->pending_request = retry_req;
                    agent->async_state = AGENT_ASYNC_WAITING;
                    return NULL;  // Signal: retry in progress, keep polling
                }
            }
        }

        // No retry possible - return error
        char error_buf[512];
        if (resp->error) {
            snprintf(error_buf, sizeof(error_buf), "Error: %s", resp->error);
        } else {
            snprintf(error_buf, sizeof(error_buf),
                    "Error: API returned status %d", resp->status_code);
        }
        VegaString* result = vega_string_from_cstr(error_buf);
        http_response_free(resp);
        agent->async_state = AGENT_ASYNC_IDLE;
        clear_tool_context(&agent->tool_ctx);
        return result;
    }

    // Success - record it for circuit breaker
    if (agent->process) {
        process_record_success(agent->process);
    }

    // Check for tool use
    if (resp->body && anthropic_has_tool_use(resp->body)) {
        // Check iteration limit
        if (agent->tool_ctx.iteration >= agent->tool_ctx.max_iterations) {
            http_response_free(resp);
            agent->async_state = AGENT_ASYNC_IDLE;
            clear_tool_context(&agent->tool_ctx);
            return vega_string_from_cstr("Error: Max tool iterations exceeded");
        }

        // Extract tool use info
        char* tool_id = NULL;
        char* tool_input = NULL;
        char* tool_name = anthropic_extract_tool_use(resp->body, &tool_id, &tool_input);

        if (tool_name) {
            // Execute tool (sync - local execution is fast)
            trace_tool_call(agent->agent_id, agent->name, tool_name, tool_input);
            char* tool_result = execute_tool(vm, agent, tool_name, tool_input);
            trace_tool_result(agent->agent_id, agent->name, tool_name, tool_result);

            // Extract assistant content for proper API formatting
            char* assistant_content = extract_assistant_content(resp->body);

            // Build tool definitions
            ToolDefinition* tool_defs = build_tool_defs(agent);

            // Start ASYNC request for tool result (not sync!)
            HttpAsyncRequest* req = http_async_send_tool_result_v2(
                vm->api_key,
                agent->model,
                agent->system_prompt,
                (const char**)agent->messages,
                (int)agent->message_count,
                assistant_content,
                tool_id,
                tool_result,
                tool_defs,
                (int)agent->tool_count,
                agent->temperature
            );

            // Clean up
            free(assistant_content);
            free(tool_id);
            free(tool_name);
            free(tool_input);
            free(tool_result);
            free(tool_defs);
            http_response_free(resp);

            if (req) {
                // Continue async loop
                agent->pending_request = req;
                agent->async_state = AGENT_ASYNC_WAITING;
                agent->tool_ctx.iteration++;
                return NULL;  // Signal: still processing, keep polling
            } else {
                // Failed to start follow-up request
                agent->async_state = AGENT_ASYNC_IDLE;
                clear_tool_context(&agent->tool_ctx);
                return vega_string_from_cstr("Error: Failed to send tool result");
            }
        }
    }

    // No tool use - extract final text response
    char* text = anthropic_extract_text(resp->body);
    http_response_free(resp);

    // Reset state
    agent->async_state = AGENT_ASYNC_IDLE;
    clear_tool_context(&agent->tool_ctx);

    if (text) {
        trace_msg_recv(agent->agent_id, agent->name, text, &(TokenUsage){0}, 0);
        // Best effort - history add failure is non-fatal
        (void)add_message(agent, text);
        VegaString* result = vega_string_from_cstr(text);
        free(text);
        return result;
    }

    trace_error(agent->agent_id, "No response from API");
    return vega_string_from_cstr("Error: No response from API");
}

bool agent_has_pending_request(VegaAgent* agent) {
    return agent && agent->async_state != AGENT_ASYNC_IDLE;
}

void agent_cancel_pending(VegaAgent* agent) {
    if (!agent) return;

    if (agent->pending_request) {
        http_async_cancel(agent->pending_request);
        agent->pending_request = NULL;
    }
    agent->async_state = AGENT_ASYNC_IDLE;
    clear_tool_context(&agent->tool_ctx);
}
