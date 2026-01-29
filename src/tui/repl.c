#include "repl.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// ============================================================================
// Session Lifecycle
// ============================================================================

ReplSession* repl_create(VegaVM* vm) {
    ReplSession* repl = calloc(1, sizeof(ReplSession));
    if (!repl) return NULL;

    repl->vm = vm;
    repl->history_capacity = 100;
    repl->history = calloc(repl->history_capacity, sizeof(char*));

    return repl;
}

void repl_free(ReplSession* repl) {
    if (!repl) return;

    // Free history
    for (int i = 0; i < repl->history_count; i++) {
        free(repl->history[i]);
    }
    free(repl->history);

    // Free multiline buffer
    free(repl->multiline_buffer);

    // Free last result
    free(repl->last_result);

    free(repl);
}

// ============================================================================
// Evaluation
// ============================================================================

char* repl_eval(ReplSession* repl, const char* input) {
    if (!repl || !input) return NULL;

    // Clear previous state
    free(repl->last_result);
    repl->last_result = NULL;
    repl->last_was_error = false;

    // Skip leading whitespace
    while (*input && isspace(*input)) input++;

    if (strlen(input) == 0) {
        return strdup("");
    }

    // Check for built-in commands
    char* arg = NULL;
    ReplCommandType cmd = repl_parse_command(input, &arg);

    if (cmd != REPL_CMD_NONE) {
        char* result = NULL;

        switch (cmd) {
            case REPL_CMD_HELP:
                result = strdup(
                    "REPL Commands:\n"
                    "  help       - Show this help\n"
                    "  quit/exit  - Exit the REPL\n"
                    "  clear      - Clear history\n"
                    "  history    - Show command history\n"
                    "  load FILE  - Load a .vgb file\n"
                    "  run        - Run loaded program\n"
                    "  agents     - List active agents\n"
                    "  vars       - List global variables\n"
                    "  reset      - Reset VM state\n"
                );
                break;

            case REPL_CMD_QUIT:
                result = strdup("Goodbye!");
                break;

            case REPL_CMD_HISTORY:
                if (repl->history_count == 0) {
                    result = strdup("(no history)");
                } else {
                    size_t len = 0;
                    for (int i = 0; i < repl->history_count; i++) {
                        len += strlen(repl->history[i]) + 16;
                    }
                    result = malloc(len);
                    result[0] = '\0';
                    for (int i = 0; i < repl->history_count; i++) {
                        char line[512];
                        snprintf(line, sizeof(line), "%3d: %s\n", i + 1, repl->history[i]);
                        strcat(result, line);
                    }
                }
                break;

            case REPL_CMD_AGENTS:
                if (repl->vm && repl->vm->agent_count > 0) {
                    size_t len = repl->vm->agent_count * 128;
                    result = malloc(len);
                    result[0] = '\0';
                    for (uint32_t i = 0; i < repl->vm->agent_count; i++) {
                        AgentDef* def = &repl->vm->agents[i];
                        uint32_t name_len;
                        const char* name = vm_read_string(repl->vm, def->name_idx, &name_len);
                        char line[128];
                        snprintf(line, sizeof(line), "Agent %u: %.*s\n",
                                i, (int)name_len, name);
                        strcat(result, line);
                    }
                } else {
                    result = strdup("(no agents defined)");
                }
                break;

            case REPL_CMD_VARS:
                if (repl->vm && repl->vm->global_count > 0) {
                    size_t len = repl->vm->global_count * 128;
                    result = malloc(len);
                    result[0] = '\0';
                    for (uint32_t i = 0; i < repl->vm->global_count; i++) {
                        char line[128];
                        snprintf(line, sizeof(line), "%s = ", repl->vm->global_names[i]);
                        strcat(result, line);

                        // Get value string representation
                        VegaString* str = value_to_string(repl->vm->globals[i]);
                        strncat(result, str->data, str->length);
                        strcat(result, "\n");
                        vega_obj_release(str);
                    }
                } else {
                    result = strdup("(no globals)");
                }
                break;

            case REPL_CMD_RESET:
                // Reset execution state
                repl->vm->ip = 0;
                repl->vm->sp = 0;
                repl->vm->frame_count = 0;
                repl->vm->running = true;
                repl->vm->had_error = false;
                result = strdup("VM state reset.");
                break;

            case REPL_CMD_LOAD:
                if (arg) {
                    if (vm_load_file(repl->vm, arg)) {
                        char buf[256];
                        snprintf(buf, sizeof(buf),
                                "Loaded: %u functions, %u agents",
                                repl->vm->func_count, repl->vm->agent_count);
                        result = strdup(buf);
                    } else {
                        repl->last_was_error = true;
                        char buf[256];
                        snprintf(buf, sizeof(buf), "Error: %s", vm_error_msg(repl->vm));
                        result = strdup(buf);
                    }
                } else {
                    repl->last_was_error = true;
                    result = strdup("Usage: load <filename>");
                }
                break;

            case REPL_CMD_RUN:
                if (repl->vm && repl->vm->code_size > 0) {
                    bool success = vm_run(repl->vm);
                    if (success) {
                        result = strdup("Program completed.");
                    } else {
                        repl->last_was_error = true;
                        char buf[256];
                        snprintf(buf, sizeof(buf), "Error: %s", vm_error_msg(repl->vm));
                        result = strdup(buf);
                    }
                } else {
                    repl->last_was_error = true;
                    result = strdup("No program loaded.");
                }
                break;

            default:
                result = strdup("Unknown command");
                break;
        }

        free(arg);

        // Add to history
        if (repl->history_count < repl->history_capacity) {
            repl->history[repl->history_count++] = strdup(input);
        }

        repl->last_result = result;
        return result ? strdup(result) : NULL;
    }

    // Not a built-in command - currently we don't support runtime compilation
    // This would require linking the compiler into the VM
    repl->last_was_error = true;
    repl->last_result = strdup("Runtime evaluation not yet supported. Use 'load' to load a .vgb file.");
    return strdup(repl->last_result);
}

bool repl_needs_more(ReplSession* repl, const char* input) {
    if (!input) return false;

    // Check for unclosed braces
    int brace_count = 0;
    bool in_string = false;

    for (const char* p = input; *p; p++) {
        if (*p == '"' && (p == input || *(p-1) != '\\')) {
            in_string = !in_string;
        } else if (!in_string) {
            if (*p == '{') brace_count++;
            else if (*p == '}') brace_count--;
        }
    }

    return brace_count > 0 || repl->in_multiline;
}

const char* repl_get_error(ReplSession* repl) {
    if (!repl || !repl->last_was_error) return NULL;
    return repl->last_result;
}

bool repl_was_error(ReplSession* repl) {
    return repl && repl->last_was_error;
}

void repl_clear_error(ReplSession* repl) {
    if (repl) {
        repl->last_was_error = false;
    }
}

void repl_append_line(ReplSession* repl, const char* line) {
    if (!repl || !line) return;

    size_t line_len = strlen(line);
    size_t needed = repl->multiline_len + line_len + 2;  // +2 for newline and null

    if (needed > repl->multiline_capacity) {
        repl->multiline_capacity = needed * 2;
        repl->multiline_buffer = realloc(repl->multiline_buffer, repl->multiline_capacity);
    }

    if (repl->multiline_len > 0) {
        repl->multiline_buffer[repl->multiline_len++] = '\n';
    }
    memcpy(repl->multiline_buffer + repl->multiline_len, line, line_len);
    repl->multiline_len += line_len;
    repl->multiline_buffer[repl->multiline_len] = '\0';

    repl->in_multiline = true;
}

const char* repl_get_multiline(ReplSession* repl) {
    return repl ? repl->multiline_buffer : NULL;
}

void repl_clear_multiline(ReplSession* repl) {
    if (repl) {
        repl->multiline_len = 0;
        if (repl->multiline_buffer) {
            repl->multiline_buffer[0] = '\0';
        }
        repl->in_multiline = false;
    }
}

// ============================================================================
// Command Parsing
// ============================================================================

ReplCommandType repl_parse_command(const char* input, char** arg) {
    if (!input) return REPL_CMD_NONE;

    // Skip whitespace
    while (*input && isspace(*input)) input++;

    char cmd[64] = {0};
    char arg_buf[512] = {0};

    int n = sscanf(input, "%63s %511[^\n]", cmd, arg_buf);
    (void)n;

    if (arg && arg_buf[0]) {
        *arg = strdup(arg_buf);
    } else if (arg) {
        *arg = NULL;
    }

    // Match commands
    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        return REPL_CMD_HELP;
    }
    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0 || strcmp(cmd, "q") == 0) {
        return REPL_CMD_QUIT;
    }
    if (strcmp(cmd, "clear") == 0) {
        return REPL_CMD_CLEAR;
    }
    if (strcmp(cmd, "history") == 0 || strcmp(cmd, "hist") == 0) {
        return REPL_CMD_HISTORY;
    }
    if (strcmp(cmd, "load") == 0) {
        return REPL_CMD_LOAD;
    }
    if (strcmp(cmd, "run") == 0) {
        return REPL_CMD_RUN;
    }
    if (strcmp(cmd, "agents") == 0) {
        return REPL_CMD_AGENTS;
    }
    if (strcmp(cmd, "vars") == 0 || strcmp(cmd, "globals") == 0) {
        return REPL_CMD_VARS;
    }
    if (strcmp(cmd, "reset") == 0) {
        return REPL_CMD_RESET;
    }

    return REPL_CMD_NONE;
}
