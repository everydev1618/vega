#include "vm.h"
#include "agent.h"
#include "process.h"
#include "scheduler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ============================================================================
// Initialization
// ============================================================================

void vm_init(VegaVM* vm) {
    memset(vm, 0, sizeof(VegaVM));
    vm->api_key = getenv("ANTHROPIC_API_KEY");
    vm->next_pid = 1;  // PID 0 reserved for "no parent"
    scheduler_init(&vm->scheduler, vm->processes, &vm->process_count);
}

void vm_free(VegaVM* vm) {
    free(vm->code);
    free(vm->constants);
    free(vm->functions);
    free(vm->agents);

    // Release global values
    for (uint32_t i = 0; i < vm->global_count; i++) {
        value_release(vm->globals[i]);
        free(vm->global_names[i]);
    }

    // Release stack values
    for (uint32_t i = 0; i < vm->sp; i++) {
        value_release(vm->stack[i]);
    }

    // Free processes
    for (uint32_t i = 0; i < vm->process_count; i++) {
        process_free(vm->processes[i]);
    }

    // Cleanup scheduler
    scheduler_cleanup(&vm->scheduler);
}

// ============================================================================
// Loading
// ============================================================================

bool vm_load_file(VegaVM* vm, const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        snprintf(vm->error_msg, sizeof(vm->error_msg),
                "Cannot open file: %s", filename);
        vm->had_error = true;
        return false;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    uint8_t* data = malloc(size);
    if (!data) {
        fclose(f);
        snprintf(vm->error_msg, sizeof(vm->error_msg), "Out of memory");
        vm->had_error = true;
        return false;
    }

    fread(data, 1, size, f);
    fclose(f);

    bool result = vm_load(vm, data, size);
    free(data);
    return result;
}

bool vm_load(VegaVM* vm, uint8_t* bytecode, uint32_t size) {
    if (size < sizeof(VegaHeader)) {
        snprintf(vm->error_msg, sizeof(vm->error_msg), "Invalid bytecode: too small");
        vm->had_error = true;
        return false;
    }

    // Read header
    VegaHeader* header = (VegaHeader*)bytecode;

    if (header->magic != VEGA_MAGIC) {
        snprintf(vm->error_msg, sizeof(vm->error_msg),
                "Invalid bytecode: bad magic number");
        vm->had_error = true;
        return false;
    }

    if (header->version != VEGA_VERSION) {
        snprintf(vm->error_msg, sizeof(vm->error_msg),
                "Bytecode version mismatch: expected %u, got %u",
                VEGA_VERSION, header->version);
        vm->had_error = true;
        return false;
    }

    uint8_t* ptr = bytecode + sizeof(VegaHeader);

    // Read function and agent counts
    uint16_t func_count = *(uint16_t*)ptr; ptr += 2;
    uint16_t agent_count = *(uint16_t*)ptr; ptr += 2;

    // Read function table
    vm->func_count = func_count;
    vm->functions = malloc(func_count * sizeof(FunctionDef));
    memcpy(vm->functions, ptr, func_count * sizeof(FunctionDef));
    ptr += func_count * sizeof(FunctionDef);

    // Read agent definitions
    vm->agent_count = agent_count;
    vm->agents = malloc(agent_count * sizeof(AgentDef));
    memcpy(vm->agents, ptr, agent_count * sizeof(AgentDef));
    ptr += agent_count * sizeof(AgentDef);

    // Read constant pool
    vm->const_size = header->const_pool_size;
    vm->constants = malloc(vm->const_size);
    memcpy(vm->constants, ptr, vm->const_size);
    ptr += vm->const_size;

    // Read code
    vm->code_size = header->code_size;
    vm->code = malloc(vm->code_size);
    memcpy(vm->code, ptr, vm->code_size);

    return true;
}

// ============================================================================
// Stack Operations
// ============================================================================

void vm_push(VegaVM* vm, Value v) {
    if (vm->sp >= VM_STACK_MAX) {
        snprintf(vm->error_msg, sizeof(vm->error_msg), "Stack overflow");
        vm->had_error = true;
        vm->running = false;
        return;
    }
    vm->stack[vm->sp++] = v;
}

Value vm_pop(VegaVM* vm) {
    if (vm->sp == 0) {
        snprintf(vm->error_msg, sizeof(vm->error_msg), "Stack underflow");
        vm->had_error = true;
        vm->running = false;
        return value_null();
    }
    return vm->stack[--vm->sp];
}

Value vm_peek(VegaVM* vm, uint32_t distance) {
    if (distance >= vm->sp) {
        return value_null();
    }
    return vm->stack[vm->sp - 1 - distance];
}

// ============================================================================
// Constant Pool Access
// ============================================================================

Value vm_read_constant(VegaVM* vm, uint16_t index) {
    if (index >= vm->const_size) {
        return value_null();
    }

    uint8_t* ptr = vm->constants + index;
    uint8_t type = *ptr++;

    switch (type) {
        case CONST_INT: {
            int32_t val;
            memcpy(&val, ptr, 4);
            return value_int(val);
        }
        case CONST_STRING: {
            uint16_t len = ptr[0] | (ptr[1] << 8);
            ptr += 2;
            VegaString* str = vega_string_new((char*)ptr, len);
            return value_string(str);
        }
        case CONST_FLOAT: {
            double val;
            memcpy(&val, ptr, 8);
            return value_float(val);
        }
        default:
            return value_null();
    }
}

const char* vm_read_string(VegaVM* vm, uint16_t index, uint32_t* out_len) {
    if (index >= vm->const_size) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    uint8_t* ptr = vm->constants + index;
    if (*ptr != CONST_STRING) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    ptr++;

    uint16_t len = ptr[0] | (ptr[1] << 8);
    ptr += 2;

    if (out_len) *out_len = len;
    return (const char*)ptr;
}

// ============================================================================
// Global Variables
// ============================================================================

Value vm_get_global(VegaVM* vm, const char* name) {
    for (uint32_t i = 0; i < vm->global_count; i++) {
        if (strcmp(vm->global_names[i], name) == 0) {
            return vm->globals[i];
        }
    }
    return value_null();
}

void vm_set_global(VegaVM* vm, const char* name, Value v) {
    // Check if exists
    for (uint32_t i = 0; i < vm->global_count; i++) {
        if (strcmp(vm->global_names[i], name) == 0) {
            value_release(vm->globals[i]);
            value_retain(v);
            vm->globals[i] = v;
            return;
        }
    }

    // Add new
    if (vm->global_count < VM_GLOBALS_MAX) {
        vm->global_names[vm->global_count] = strdup(name);
        value_retain(v);
        vm->globals[vm->global_count] = v;
        vm->global_count++;
    }
}

// ============================================================================
// Lookups
// ============================================================================

int vm_find_function(VegaVM* vm, const char* name) {
    for (uint32_t i = 0; i < vm->func_count; i++) {
        uint32_t len;
        const char* fn_name = vm_read_string(vm, vm->functions[i].name_idx, &len);
        if (fn_name && strncmp(fn_name, name, len) == 0 && strlen(name) == len) {
            return (int)i;
        }
    }
    return -1;
}

int vm_find_agent(VegaVM* vm, const char* name) {
    for (uint32_t i = 0; i < vm->agent_count; i++) {
        uint32_t len;
        const char* ag_name = vm_read_string(vm, vm->agents[i].name_idx, &len);
        if (ag_name && strncmp(ag_name, name, len) == 0 && strlen(name) == len) {
            return (int)i;
        }
    }
    return -1;
}

AgentDef* vm_get_agent(VegaVM* vm, uint32_t index) {
    if (index >= vm->agent_count) return NULL;
    return &vm->agents[index];
}

// ============================================================================
// Native Calls
// ============================================================================

static Value call_native(VegaVM* vm, const char* name, Value* args, uint32_t argc) {
    // file::read(path) -> str
    if (strcmp(name, "file::read") == 0 && argc == 1) {
        if (args[0].type != VAL_STRING) {
            return value_null();
        }
        FILE* f = fopen(args[0].as.string->data, "r");
        if (!f) return value_null();

        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        char* content = malloc(size + 1);
        size_t read = fread(content, 1, size, f);
        content[read] = '\0';
        fclose(f);

        VegaString* str = vega_string_new(content, (uint32_t)read);
        free(content);
        return value_string(str);
    }

    // file::write(path, content) -> void
    if (strcmp(name, "file::write") == 0 && argc == 2) {
        if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
            return value_null();
        }
        FILE* f = fopen(args[0].as.string->data, "w");
        if (!f) return value_null();
        fwrite(args[1].as.string->data, 1, args[1].as.string->length, f);
        fclose(f);
        return value_null();
    }

    // file::exists(path) -> bool
    if (strcmp(name, "file::exists") == 0 && argc == 1) {
        if (args[0].type != VAL_STRING) {
            return value_bool(false);
        }
        FILE* f = fopen(args[0].as.string->data, "r");
        if (f) {
            fclose(f);
            return value_bool(true);
        }
        return value_bool(false);
    }

    // str::len(s) -> int
    if (strcmp(name, "str::len") == 0 && argc == 1) {
        if (args[0].type != VAL_STRING) return value_int(0);
        return value_int(args[0].as.string->length);
    }

    // str::contains(s, substr) -> bool
    if (strcmp(name, "str::contains") == 0 && argc == 2) {
        if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
            return value_bool(false);
        }
        return value_bool(vega_string_contains(args[0].as.string, args[1].as.string));
    }

    return value_null();
}

// ============================================================================
// Execution
// ============================================================================

bool vm_step(VegaVM* vm) {
    if (!vm->running || vm->ip >= vm->code_size) {
        return false;
    }

    uint8_t op = vm->code[vm->ip++];

    switch (op) {
        case OP_NOP:
            break;

        case OP_PUSH_CONST: {
            uint16_t idx = READ_U16(vm->code, vm->ip);
            vm->ip += 2;
            Value v = vm_read_constant(vm, idx);
            vm_push(vm, v);
            break;
        }

        case OP_PUSH_INT: {
            int32_t val = (int32_t)READ_U32(vm->code, vm->ip);
            vm->ip += 4;
            vm_push(vm, value_int(val));
            break;
        }

        case OP_PUSH_TRUE:
            vm_push(vm, value_bool(true));
            break;

        case OP_PUSH_FALSE:
            vm_push(vm, value_bool(false));
            break;

        case OP_PUSH_NULL:
            vm_push(vm, value_null());
            break;

        case OP_POP:
            value_release(vm_pop(vm));
            break;

        case OP_DUP: {
            Value v = vm_peek(vm, 0);
            value_retain(v);
            vm_push(vm, v);
            break;
        }

        case OP_LOAD_LOCAL: {
            uint8_t slot = vm->code[vm->ip++];
            uint32_t bp = vm->frame_count > 0 ?
                vm->frames[vm->frame_count - 1].bp : 0;
            Value v = vm->stack[bp + slot];
            value_retain(v);
            vm_push(vm, v);
            break;
        }

        case OP_STORE_LOCAL: {
            uint8_t slot = vm->code[vm->ip++];
            uint32_t bp = vm->frame_count > 0 ?
                vm->frames[vm->frame_count - 1].bp : 0;
            Value v = vm_pop(vm);
            value_release(vm->stack[bp + slot]);
            vm->stack[bp + slot] = v;
            break;
        }

        case OP_LOAD_GLOBAL: {
            uint16_t idx = READ_U16(vm->code, vm->ip);
            vm->ip += 2;
            uint32_t len;
            const char* name = vm_read_string(vm, idx, &len);
            char* name_z = strndup(name, len);

            // First try to find a global variable
            Value v = vm_get_global(vm, name_z);
            if (v.type == VAL_NULL) {
                // If not found, check if it's a function name
                int func_id = vm_find_function(vm, name_z);
                if (func_id >= 0) {
                    v = value_function((uint32_t)func_id);
                }
            }

            value_retain(v);
            vm_push(vm, v);
            free(name_z);
            break;
        }

        case OP_STORE_GLOBAL: {
            uint16_t idx = READ_U16(vm->code, vm->ip);
            vm->ip += 2;
            uint32_t len;
            const char* name = vm_read_string(vm, idx, &len);
            char* name_z = strndup(name, len);
            Value v = vm_pop(vm);
            vm_set_global(vm, name_z, v);
            value_release(v);
            free(name_z);
            break;
        }

        case OP_ADD: {
            Value b = vm_pop(vm);
            Value a = vm_pop(vm);
            vm_push(vm, value_add(a, b));
            value_release(a);
            value_release(b);
            break;
        }

        case OP_SUB: {
            Value b = vm_pop(vm);
            Value a = vm_pop(vm);
            vm_push(vm, value_sub(a, b));
            break;
        }

        case OP_MUL: {
            Value b = vm_pop(vm);
            Value a = vm_pop(vm);
            vm_push(vm, value_mul(a, b));
            break;
        }

        case OP_DIV: {
            Value b = vm_pop(vm);
            Value a = vm_pop(vm);
            vm_push(vm, value_div(a, b));
            break;
        }

        case OP_MOD: {
            Value b = vm_pop(vm);
            Value a = vm_pop(vm);
            vm_push(vm, value_mod(a, b));
            break;
        }

        case OP_NEG: {
            Value v = vm_pop(vm);
            vm_push(vm, value_neg(v));
            break;
        }

        case OP_EQ: {
            Value b = vm_pop(vm);
            Value a = vm_pop(vm);
            vm_push(vm, value_bool(value_equals(a, b)));
            value_release(a);
            value_release(b);
            break;
        }

        case OP_NE: {
            Value b = vm_pop(vm);
            Value a = vm_pop(vm);
            vm_push(vm, value_bool(!value_equals(a, b)));
            value_release(a);
            value_release(b);
            break;
        }

        case OP_LT: {
            Value b = vm_pop(vm);
            Value a = vm_pop(vm);
            vm_push(vm, value_bool(value_compare(a, b) < 0));
            break;
        }

        case OP_LE: {
            Value b = vm_pop(vm);
            Value a = vm_pop(vm);
            vm_push(vm, value_bool(value_compare(a, b) <= 0));
            break;
        }

        case OP_GT: {
            Value b = vm_pop(vm);
            Value a = vm_pop(vm);
            vm_push(vm, value_bool(value_compare(a, b) > 0));
            break;
        }

        case OP_GE: {
            Value b = vm_pop(vm);
            Value a = vm_pop(vm);
            vm_push(vm, value_bool(value_compare(a, b) >= 0));
            break;
        }

        case OP_NOT: {
            Value v = vm_pop(vm);
            vm_push(vm, value_bool(!value_is_truthy(v)));
            value_release(v);
            break;
        }

        case OP_AND: {
            Value b = vm_pop(vm);
            Value a = vm_pop(vm);
            vm_push(vm, value_bool(value_is_truthy(a) && value_is_truthy(b)));
            value_release(a);
            value_release(b);
            break;
        }

        case OP_OR: {
            Value b = vm_pop(vm);
            Value a = vm_pop(vm);
            vm_push(vm, value_bool(value_is_truthy(a) || value_is_truthy(b)));
            value_release(a);
            value_release(b);
            break;
        }

        case OP_JUMP: {
            int16_t offset = READ_I16(vm->code, vm->ip);
            vm->ip += 2 + offset;
            break;
        }

        case OP_JUMP_IF: {
            int16_t offset = READ_I16(vm->code, vm->ip);
            vm->ip += 2;
            Value cond = vm_pop(vm);
            if (value_is_truthy(cond)) {
                vm->ip += offset;
            }
            value_release(cond);
            break;
        }

        case OP_JUMP_IF_NOT: {
            int16_t offset = READ_I16(vm->code, vm->ip);
            vm->ip += 2;
            Value cond = vm_pop(vm);
            if (!value_is_truthy(cond)) {
                vm->ip += offset;
            }
            value_release(cond);
            break;
        }

        case OP_CALL: {
            uint8_t argc = vm->code[vm->ip++];
            Value callee = vm_pop(vm);

            if (callee.type != VAL_FUNCTION) {
                snprintf(vm->error_msg, sizeof(vm->error_msg),
                        "Cannot call non-function");
                vm->had_error = true;
                vm->running = false;
                break;
            }

            uint32_t func_id = callee.as.function_id;
            if (func_id >= vm->func_count) {
                snprintf(vm->error_msg, sizeof(vm->error_msg),
                        "Invalid function id: %u", func_id);
                vm->had_error = true;
                vm->running = false;
                break;
            }

            FunctionDef* fn = &vm->functions[func_id];

            // Push call frame
            if (vm->frame_count >= VM_FRAMES_MAX) {
                snprintf(vm->error_msg, sizeof(vm->error_msg),
                        "Call stack overflow");
                vm->had_error = true;
                vm->running = false;
                break;
            }

            CallFrame* frame = &vm->frames[vm->frame_count++];
            frame->function_id = func_id;
            frame->ip = vm->ip;
            frame->bp = vm->sp - argc;

            // Reserve space for locals
            while (vm->sp < frame->bp + fn->local_count) {
                vm_push(vm, value_null());
            }

            vm->ip = fn->code_offset;
            break;
        }

        case OP_RETURN: {
            Value result = vm_pop(vm);

            if (vm->frame_count == 0) {
                vm->running = false;
                vm_push(vm, result);
                break;
            }

            CallFrame* frame = &vm->frames[--vm->frame_count];

            // Pop locals and arguments
            while (vm->sp > frame->bp) {
                value_release(vm_pop(vm));
            }

            vm->ip = frame->ip;
            vm_push(vm, result);
            break;
        }

        case OP_CALL_NATIVE: {
            uint16_t idx = READ_U16(vm->code, vm->ip);
            vm->ip += 2;

            uint32_t len;
            const char* name = vm_read_string(vm, idx, &len);
            char* name_z = strndup(name, len);

            // Count arguments (we need to peek backwards)
            // For simplicity, assume max 4 args
            Value args[4];
            uint32_t argc = 0;

            // The native call should have args on stack
            // We'll pop them
            if (strcmp(name_z, "file::read") == 0 ||
                strcmp(name_z, "file::exists") == 0 ||
                strcmp(name_z, "str::len") == 0) {
                argc = 1;
                args[0] = vm_pop(vm);
            } else if (strcmp(name_z, "file::write") == 0 ||
                       strcmp(name_z, "str::contains") == 0) {
                argc = 2;
                args[1] = vm_pop(vm);
                args[0] = vm_pop(vm);
            }

            Value result = call_native(vm, name_z, args, argc);
            vm_push(vm, result);

            for (uint32_t i = 0; i < argc; i++) {
                value_release(args[i]);
            }

            free(name_z);
            break;
        }

        case OP_SPAWN_AGENT: {
            uint16_t idx = READ_U16(vm->code, vm->ip);
            vm->ip += 2;

            uint32_t len;
            const char* name = vm_read_string(vm, idx, &len);
            char* name_z = strndup(name, len);

            int agent_idx = vm_find_agent(vm, name_z);
            if (agent_idx < 0) {
                snprintf(vm->error_msg, sizeof(vm->error_msg),
                        "Unknown agent: %s", name_z);
                vm->had_error = true;
                vm->running = false;
                free(name_z);
                break;
            }

            VegaAgent* agent = agent_spawn(vm, (uint32_t)agent_idx);
            vm_push(vm, value_agent(agent));
            free(name_z);
            break;
        }

        case OP_SPAWN_SUPERVISED: {
            uint16_t idx = READ_U16(vm->code, vm->ip);
            vm->ip += 2;

            // Read supervision config
            uint8_t strategy = vm->code[vm->ip++];
            uint32_t max_restarts = READ_U32(vm->code, vm->ip);
            vm->ip += 4;
            uint32_t window_ms = READ_U32(vm->code, vm->ip);
            vm->ip += 4;

            uint32_t len;
            const char* name = vm_read_string(vm, idx, &len);
            char* name_z = strndup(name, len);

            int agent_idx = vm_find_agent(vm, name_z);
            if (agent_idx < 0) {
                snprintf(vm->error_msg, sizeof(vm->error_msg),
                        "Unknown agent: %s", name_z);
                vm->had_error = true;
                vm->running = false;
                free(name_z);
                break;
            }

            // Create supervision config
            SupervisionConfig config = {
                .strategy = (RestartStrategy)strategy,
                .max_restarts = max_restarts,
                .window_ms = window_ms,
                .restart_count = 0,
                .window_start = 0
            };

            // Spawn supervised agent (creates both agent and process)
            VegaAgent* agent = agent_spawn_supervised(vm, (uint32_t)agent_idx, &config);

            vm_push(vm, value_agent(agent));
            free(name_z);
            break;
        }

        case OP_YIELD: {
            // Yield to scheduler (for cooperative multitasking)
            // For now, this is a no-op since we don't have full process integration yet
            break;
        }

        case OP_SEND_MSG: {
            Value msg = vm_pop(vm);
            Value target = vm_pop(vm);

            if (target.type != VAL_AGENT || !target.as.agent) {
                snprintf(vm->error_msg, sizeof(vm->error_msg),
                        "Cannot send message to non-agent");
                vm->had_error = true;
                vm->running = false;
                break;
            }

            VegaString* msg_str = value_to_string(msg);
            VegaString* response = agent_send_message(vm, target.as.agent, msg_str->data);

            value_release(msg);
            vega_obj_release(msg_str);

            if (response) {
                vm_push(vm, value_string(response));
            } else {
                vm_push(vm, value_null());
            }
            break;
        }

        case OP_STR_HAS: {
            Value substr = vm_pop(vm);
            Value str = vm_pop(vm);

            bool result = false;
            if (str.type == VAL_STRING && substr.type == VAL_STRING) {
                result = vega_string_contains(str.as.string, substr.as.string);
            }

            vm_push(vm, value_bool(result));
            value_release(str);
            value_release(substr);
            break;
        }

        case OP_CALL_METHOD: {
            uint16_t name_idx = READ_U16(vm->code, vm->ip);
            vm->ip += 2;
            uint8_t argc = vm->code[vm->ip++];

            uint32_t len;
            const char* method = vm_read_string(vm, name_idx, &len);

            // Pop args
            Value args[8];
            for (int i = argc - 1; i >= 0; i--) {
                args[i] = vm_pop(vm);
            }
            Value obj = vm_pop(vm);

            // String methods
            if (obj.type == VAL_STRING && obj.as.string) {
                if (strncmp(method, "has", len) == 0 && argc == 1) {
                    bool result = false;
                    if (args[0].type == VAL_STRING) {
                        result = vega_string_contains(obj.as.string, args[0].as.string);
                    }
                    vm_push(vm, value_bool(result));
                } else if (strncmp(method, "len", len) == 0 && argc == 0) {
                    vm_push(vm, value_int(obj.as.string->length));
                } else {
                    vm_push(vm, value_null());
                }
            } else {
                vm_push(vm, value_null());
            }

            value_release(obj);
            for (uint32_t i = 0; i < argc; i++) {
                value_release(args[i]);
            }
            break;
        }

        case OP_PRINT: {
            Value v = vm_pop(vm);
            value_print(v);
            printf("\n");
            value_release(v);
            // Push null as return value (for expression statement semantics)
            vm_push(vm, value_null());
            break;
        }

        case OP_HALT:
            vm->running = false;
            break;

        default:
            snprintf(vm->error_msg, sizeof(vm->error_msg),
                    "Unknown opcode: 0x%02x at %u", op, vm->ip - 1);
            vm->had_error = true;
            vm->running = false;
            break;
    }

    return vm->running;
}

bool vm_run(VegaVM* vm) {
    // Check for API key
    if (!vm->api_key) {
        fprintf(stderr, "Warning: ANTHROPIC_API_KEY not set\n");
    }

    // Find main function
    int main_id = vm_find_function(vm, "main");
    if (main_id < 0) {
        snprintf(vm->error_msg, sizeof(vm->error_msg),
                "No main function found");
        vm->had_error = true;
        return false;
    }

    // Set up initial call frame for main
    FunctionDef* main_fn = &vm->functions[main_id];
    vm->ip = main_fn->code_offset;
    vm->running = true;

    // Reserve space for main's locals
    while (vm->sp < main_fn->local_count) {
        vm_push(vm, value_null());
    }

    // Run until done
    while (vm_step(vm)) {
        // Continue
    }

    return !vm->had_error;
}

// ============================================================================
// Error Handling
// ============================================================================

bool vm_had_error(VegaVM* vm) {
    return vm->had_error;
}

const char* vm_error_msg(VegaVM* vm) {
    return vm->error_msg;
}

// ============================================================================
// Debug
// ============================================================================

void vm_print_stack(VegaVM* vm) {
    printf("Stack [%u]: ", vm->sp);
    for (uint32_t i = 0; i < vm->sp; i++) {
        value_print(vm->stack[i]);
        printf(" ");
    }
    printf("\n");
}

// ============================================================================
// Process Execution (Phase 2)
// ============================================================================

void vm_execute_process(VegaVM* vm, VegaProcess* proc) {
    if (!proc || proc->state != PROC_RUNNING) return;

    // Save current VM state
    uint32_t saved_ip = vm->ip;
    uint32_t saved_sp = vm->sp;
    uint32_t saved_frame_count = vm->frame_count;

    // Load process state into VM
    vm->ip = proc->ip;

    // Copy process stack to VM stack
    for (uint32_t i = 0; i < proc->sp; i++) {
        vm->stack[i] = proc->stack[i];
    }
    vm->sp = proc->sp;

    // Copy frames
    for (uint32_t i = 0; i < proc->frame_count; i++) {
        vm->frames[i].function_id = proc->frames[i].function_id;
        vm->frames[i].ip = proc->frames[i].ip;
        vm->frames[i].bp = proc->frames[i].bp;
    }
    vm->frame_count = proc->frame_count;

    vm->running = true;

    // Execute until yield, block, or exit
    while (vm->running && proc->state == PROC_RUNNING) {
        if (!vm_step(vm)) {
            // Process has exited
            if (vm->had_error) {
                process_exit(vm, proc, EXIT_ERROR, vm->error_msg);
            } else {
                process_exit(vm, proc, EXIT_NORMAL, NULL);
            }
            vm->scheduler.processes_exited++;
            break;
        }
    }

    // Save process state from VM
    proc->ip = vm->ip;
    proc->sp = vm->sp;
    for (uint32_t i = 0; i < vm->sp; i++) {
        proc->stack[i] = vm->stack[i];
    }
    proc->frame_count = vm->frame_count;
    for (uint32_t i = 0; i < vm->frame_count; i++) {
        proc->frames[i].function_id = vm->frames[i].function_id;
        proc->frames[i].ip = vm->frames[i].ip;
        proc->frames[i].bp = vm->frames[i].bp;
    }

    // Restore VM state
    vm->ip = saved_ip;
    vm->sp = saved_sp;
    vm->frame_count = saved_frame_count;
}
