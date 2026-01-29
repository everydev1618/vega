#include "vm.h"
#include "agent.h"
#include "http.h"
#include "process.h"
#include "scheduler.h"
#include "../tui/trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <unistd.h>

// ============================================================================
// Configuration
// ============================================================================

// Read a value from ~/.vega config file
// Returns allocated string or NULL if not found
static char* read_config_value(const char* key) {
    // Get home directory
    const char* home = getenv("HOME");
    if (!home) {
        struct passwd* pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return NULL;

    // Build path to ~/.vega
    char path[512];
    snprintf(path, sizeof(path), "%s/.vega", home);

    FILE* f = fopen(path, "r");
    if (!f) return NULL;

    char line[1024];
    char* result = NULL;
    size_t key_len = strlen(key);

    while (fgets(line, sizeof(line), f)) {
        // Skip comments and empty lines
        if (line[0] == '#' || line[0] == '\n') continue;

        // Check if line starts with key=
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            char* value = line + key_len + 1;
            // Trim trailing newline
            size_t len = strlen(value);
            while (len > 0 && (value[len-1] == '\n' || value[len-1] == '\r')) {
                value[--len] = '\0';
            }
            result = strdup(value);
            break;
        }
    }

    fclose(f);
    return result;
}

// Get API key from environment or config file
static char* get_api_key(void) {
    // 1. Check environment variable first (highest priority)
    const char* env_key = getenv("ANTHROPIC_API_KEY");
    if (env_key && env_key[0] != '\0') {
        return strdup(env_key);
    }

    // 2. Check ~/.vega config file
    return read_config_value("ANTHROPIC_API_KEY");
}

// ============================================================================
// Initialization
// ============================================================================

void vm_init(VegaVM* vm) {
    memset(vm, 0, sizeof(VegaVM));
    vm->api_key = get_api_key();
    vm->next_pid = 1;  // PID 0 reserved for "no parent"
    scheduler_init(&vm->scheduler, vm->processes, &vm->process_count);
}

void vm_free(VegaVM* vm) {
    // Cancel any pending async request
    if (vm->waiting_for_agent) {
        agent_cancel_pending(vm->waiting_for_agent);
        vm->waiting_for_agent = NULL;
    }
    value_release(vm->waiting_msg);
    vm->waiting_msg = value_null();

    free(vm->code);
    free(vm->constants);
    free(vm->functions);
    free(vm->agents);
    free(vm->api_key);

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
// Budget Management
// ============================================================================

// Pricing per million tokens (as of Jan 2025 for Claude Sonnet 4)
#define PRICE_INPUT_PER_MTOK    3.00   // $3.00 per million input tokens
#define PRICE_OUTPUT_PER_MTOK   15.00  // $15.00 per million output tokens
#define PRICE_CACHE_READ_PER_MTOK  0.30   // $0.30 per million cache read tokens
#define PRICE_CACHE_WRITE_PER_MTOK 3.75   // $3.75 per million cache write tokens

void vm_set_budget_input_tokens(VegaVM* vm, uint64_t max_tokens) {
    vm->budget_max_input_tokens = max_tokens;
}

void vm_set_budget_output_tokens(VegaVM* vm, uint64_t max_tokens) {
    vm->budget_max_output_tokens = max_tokens;
}

void vm_set_budget_cost(VegaVM* vm, double max_cost_usd) {
    vm->budget_max_cost_usd = max_cost_usd;
}

void vm_add_token_usage(VegaVM* vm, uint32_t input, uint32_t output) {
    vm->budget_used_input_tokens += input;
    vm->budget_used_output_tokens += output;

    // Calculate cost
    double input_cost = (input / 1000000.0) * PRICE_INPUT_PER_MTOK;
    double output_cost = (output / 1000000.0) * PRICE_OUTPUT_PER_MTOK;
    vm->budget_used_cost_usd += input_cost + output_cost;
}

double vm_get_current_cost(VegaVM* vm) {
    return vm->budget_used_cost_usd;
}

bool vm_budget_exceeded(VegaVM* vm) {
    // Check input token limit
    if (vm->budget_max_input_tokens > 0 &&
        vm->budget_used_input_tokens >= vm->budget_max_input_tokens) {
        return true;
    }

    // Check output token limit
    if (vm->budget_max_output_tokens > 0 &&
        vm->budget_used_output_tokens >= vm->budget_max_output_tokens) {
        return true;
    }

    // Check cost limit
    if (vm->budget_max_cost_usd > 0.0 &&
        vm->budget_used_cost_usd >= vm->budget_max_cost_usd) {
        return true;
    }

    return false;
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

// Search constant pool for a key string and return the string that follows it
const char* vm_find_string_after_key(VegaVM* vm, const char* key, uint32_t* out_len) {
    size_t key_len = strlen(key);
    uint32_t offset = 0;

    while (offset < vm->const_size) {
        uint8_t type = vm->constants[offset++];
        if (type == CONST_STRING) {
            uint16_t len = vm->constants[offset] | (vm->constants[offset + 1] << 8);
            offset += 2;
            const char* str = (const char*)(vm->constants + offset);

            // Check if this string matches the key
            if (len == key_len && strncmp(str, key, len) == 0) {
                // Found the key, now read the next string
                offset += len;
                if (offset < vm->const_size && vm->constants[offset] == CONST_STRING) {
                    offset++;
                    uint16_t val_len = vm->constants[offset] | (vm->constants[offset + 1] << 8);
                    offset += 2;
                    if (out_len) *out_len = val_len;
                    return (const char*)(vm->constants + offset);
                }
                break;
            }
            offset += len;
        } else if (type == CONST_INT) {
            offset += 4;  // Skip 4-byte int
        } else if (type == CONST_FLOAT) {
            offset += 8;  // Skip 8-byte float
        } else {
            break;  // Unknown type, bail out
        }
    }

    if (out_len) *out_len = 0;
    return NULL;
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

    // str::char_at(s, index) -> str (single character)
    if (strcmp(name, "str::char_at") == 0 && argc == 2) {
        if (args[0].type != VAL_STRING || args[1].type != VAL_INT) {
            return value_null();
        }
        VegaString* s = args[0].as.string;
        int64_t idx = args[1].as.integer;
        if (idx < 0 || (uint32_t)idx >= s->length) {
            return value_string(vega_string_new("", 0));
        }
        char buf[2] = {s->data[idx], '\0'};
        return value_string(vega_string_new(buf, 1));
    }

    // str::from_int(n) -> str
    if (strcmp(name, "str::from_int") == 0 && argc == 1) {
        if (args[0].type != VAL_INT) {
            return value_string(vega_string_from_cstr(""));
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "%lld", (long long)args[0].as.integer);
        return value_string(vega_string_from_cstr(buf));
    }

    // str::split(s, delimiter) -> str[] (array of strings)
    if (strcmp(name, "str::split") == 0 && argc == 2) {
        if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
            return (Value){.type = VAL_ARRAY, .as.array = array_new(0)};
        }
        VegaString* s = args[0].as.string;
        VegaString* delim = args[1].as.string;

        VegaArray* result = array_new(8);

        if (delim->length == 0) {
            // Empty delimiter - return array with original string
            array_push(result, value_string(s));
            vega_obj_retain(s);
            return (Value){.type = VAL_ARRAY, .as.array = result};
        }

        const char* start = s->data;
        const char* end = s->data + s->length;
        const char* pos;

        while ((pos = strstr(start, delim->data)) != NULL && start < end) {
            uint32_t len = pos - start;
            VegaString* part = vega_string_new(start, len);
            array_push(result, value_string(part));
            start = pos + delim->length;
        }

        // Add remaining part
        if (start <= end) {
            uint32_t len = end - start;
            VegaString* part = vega_string_new(start, len);
            array_push(result, value_string(part));
        }

        return (Value){.type = VAL_ARRAY, .as.array = result};
    }

    // str::char_code(c) -> int (ASCII value of first char)
    if (strcmp(name, "str::char_code") == 0 && argc == 1) {
        if (args[0].type != VAL_STRING || args[0].as.string->length == 0) {
            return value_int(0);
        }
        return value_int((unsigned char)args[0].as.string->data[0]);
    }

    // str::char_lower(c) -> str (lowercase single char)
    if (strcmp(name, "str::char_lower") == 0 && argc == 1) {
        if (args[0].type != VAL_STRING || args[0].as.string->length == 0) {
            return value_string(vega_string_new("", 0));
        }
        char c = args[0].as.string->data[0];
        if (c >= 'A' && c <= 'Z') {
            c = c + ('a' - 'A');
        }
        char buf[2] = {c, '\0'};
        return value_string(vega_string_new(buf, 1));
    }

    // str::split_len(s, delimiter) -> int (count of parts after split)
    if (strcmp(name, "str::split_len") == 0 && argc == 2) {
        if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
            return value_int(0);
        }
        VegaString* s = args[0].as.string;
        VegaString* delim = args[1].as.string;

        if (delim->length == 0) return value_int(1);

        int count = 1;
        const char* pos = s->data;
        while ((pos = strstr(pos, delim->data)) != NULL) {
            count++;
            pos += delim->length;
        }
        return value_int(count);
    }

    // http::get(url) -> str (response body)
    if (strcmp(name, "http::get") == 0 && argc == 1) {
        if (args[0].type != VAL_STRING) {
            return value_string(vega_string_from_cstr(""));
        }
        HttpResponse* resp = http_get(args[0].as.string->data);
        if (!resp) {
            return value_string(vega_string_from_cstr(""));
        }
        VegaString* result;
        if (resp->body) {
            result = vega_string_new(resp->body, (uint32_t)resp->body_len);
        } else if (resp->error) {
            result = vega_string_from_cstr(resp->error);
        } else {
            result = vega_string_from_cstr("");
        }
        http_response_free(resp);
        return value_string(result);
    }

    // json::get_string(json, key) -> str
    if (strcmp(name, "json::get_string") == 0 && argc == 2) {
        if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
            return value_string(vega_string_from_cstr(""));
        }
        const char* json = args[0].as.string->data;
        const char* key = args[1].as.string->data;

        // Build search pattern: "key":
        char pattern[256];
        snprintf(pattern, sizeof(pattern), "\"%s\":", key);

        const char* pos = strstr(json, pattern);
        if (!pos) return value_string(vega_string_from_cstr(""));

        pos += strlen(pattern);
        while (*pos == ' ' || *pos == '\t' || *pos == '\n') pos++;

        if (*pos != '"') return value_string(vega_string_from_cstr(""));
        pos++;

        const char* end = pos;
        while (*end && *end != '"') {
            if (*end == '\\' && *(end + 1)) end += 2;
            else end++;
        }

        VegaString* result = vega_string_new(pos, (uint32_t)(end - pos));
        return value_string(result);
    }

    // json::get_float(json, key) -> float
    if (strcmp(name, "json::get_float") == 0 && argc == 2) {
        if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
            return value_float(0.0);
        }
        const char* json = args[0].as.string->data;
        const char* key = args[1].as.string->data;

        char pattern[256];
        snprintf(pattern, sizeof(pattern), "\"%s\":", key);

        const char* pos = strstr(json, pattern);
        if (!pos) return value_float(0.0);

        pos += strlen(pattern);
        while (*pos == ' ' || *pos == '\t' || *pos == '\n') pos++;

        double val = atof(pos);
        return value_float(val);
    }

    // json::get_int(json, key) -> int
    if (strcmp(name, "json::get_int") == 0 && argc == 2) {
        if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
            return value_int(0);
        }
        const char* json = args[0].as.string->data;
        const char* key = args[1].as.string->data;

        char pattern[256];
        snprintf(pattern, sizeof(pattern), "\"%s\":", key);

        const char* pos = strstr(json, pattern);
        if (!pos) return value_int(0);

        pos += strlen(pattern);
        while (*pos == ' ' || *pos == '\t' || *pos == '\n') pos++;

        int val = atoi(pos);
        return value_int(val);
    }

    // json::get_array(json, key) -> str (array as JSON string)
    if (strcmp(name, "json::get_array") == 0 && argc == 2) {
        if (args[0].type != VAL_STRING || args[1].type != VAL_STRING) {
            return value_string(vega_string_from_cstr("[]"));
        }
        const char* json = args[0].as.string->data;
        const char* key = args[1].as.string->data;

        char pattern[256];
        snprintf(pattern, sizeof(pattern), "\"%s\":", key);

        const char* pos = strstr(json, pattern);
        if (!pos) return value_string(vega_string_from_cstr("[]"));

        pos += strlen(pattern);
        while (*pos == ' ' || *pos == '\t' || *pos == '\n') pos++;

        if (*pos != '[') return value_string(vega_string_from_cstr("[]"));

        const char* start = pos;
        int depth = 1;
        pos++;
        while (*pos && depth > 0) {
            if (*pos == '[') depth++;
            else if (*pos == ']') depth--;
            else if (*pos == '"') {
                pos++;
                while (*pos && *pos != '"') {
                    if (*pos == '\\' && *(pos + 1)) pos++;
                    pos++;
                }
            }
            pos++;
        }

        VegaString* result = vega_string_new(start, (uint32_t)(pos - start));
        return value_string(result);
    }

    // json::array_len(array_json) -> int
    if (strcmp(name, "json::array_len") == 0 && argc == 1) {
        if (args[0].type != VAL_STRING) {
            return value_int(0);
        }
        const char* json = args[0].as.string->data;
        if (!json || *json != '[') return value_int(0);

        int count = 0;
        int depth = 0;
        bool in_string = false;

        for (const char* p = json; *p; p++) {
            if (in_string) {
                if (*p == '\\' && *(p + 1)) p++;
                else if (*p == '"') in_string = false;
                continue;
            }
            if (*p == '"') in_string = true;
            else if (*p == '[') depth++;
            else if (*p == ']') depth--;
            else if (*p == ',' && depth == 1) count++;
        }

        // If we have any content, count is items - 1, so add 1
        const char* p = json + 1;
        while (*p == ' ' || *p == '\t' || *p == '\n') p++;
        if (*p != ']') count++;

        return value_int(count);
    }

    // json::array_get(array_json, index) -> str
    if (strcmp(name, "json::array_get") == 0 && argc == 2) {
        if (args[0].type != VAL_STRING || args[1].type != VAL_INT) {
            return value_string(vega_string_from_cstr(""));
        }
        const char* json = args[0].as.string->data;
        int64_t target_idx = args[1].as.integer;

        if (!json || *json != '[') return value_string(vega_string_from_cstr(""));

        int current_idx = 0;
        int depth = 0;
        bool in_string = false;
        const char* item_start = NULL;

        for (const char* p = json; *p; p++) {
            if (in_string) {
                if (*p == '\\' && *(p + 1)) p++;
                else if (*p == '"') in_string = false;
                continue;
            }
            if (*p == '"') {
                in_string = true;
                if (depth == 1 && item_start == NULL) {
                    item_start = p;
                }
            }
            else if (*p == '[') {
                depth++;
                if (depth == 1) {
                    const char* next = p + 1;
                    while (*next == ' ' || *next == '\t' || *next == '\n') next++;
                    if (*next != ']') item_start = next;
                }
            }
            else if (*p == ']') {
                if (depth == 1 && current_idx == target_idx && item_start) {
                    if (*item_start == '"') {
                        item_start++;
                        const char* end = item_start;
                        while (*end && *end != '"') {
                            if (*end == '\\' && *(end + 1)) end++;
                            end++;
                        }
                        return value_string(vega_string_new(item_start, (uint32_t)(end - item_start)));
                    }
                }
                depth--;
            }
            else if (*p == ',' && depth == 1) {
                if (current_idx == target_idx && item_start) {
                    if (*item_start == '"') {
                        item_start++;
                        const char* end = item_start;
                        while (*end && *end != '"') {
                            if (*end == '\\' && *(end + 1)) end++;
                            end++;
                        }
                        return value_string(vega_string_new(item_start, (uint32_t)(end - item_start)));
                    }
                }
                current_idx++;
                item_start = NULL;
                const char* next = p + 1;
                while (*next == ' ' || *next == '\t' || *next == '\n') next++;
                item_start = next;
            }
        }

        return value_string(vega_string_from_cstr(""));
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

    // Always poll all pending async futures first (from <~ sends)
    // This ensures all async agents make progress even during awaits
    for (uint32_t i = 0; i < vm->pending_count; i++) {
        VegaFuture* future = vm->pending_futures[i];
        if (!future || future_is_ready(future)) continue;

        VegaAgent* agent = future->agent;
        if (!agent || !agent_has_pending_request(agent)) continue;

        int poll_result = agent_poll_message(agent);
        if (poll_result == 1) {
            // Complete - get result
            VegaString* response = agent_get_message_result(vm, agent);
            if (response != NULL) {
                future_set_result(future, response);
            }
            // Note: if response is NULL, a tool loop started - we'll keep polling
        } else if (poll_result == -1) {
            // Error
            future_set_error(future, "Async request failed");
        }
        // poll_result == 0 means still pending
    }

    // Check if waiting for async agent response (synchronous send or await)
    if (vm->waiting_for_agent) {
        VegaAgent* agent = vm->waiting_for_agent;
        int poll_result = agent_poll_message(agent);
        if (poll_result == 0) {
            // Still pending - yield
            return true;
        } else if (poll_result == 1) {
            // HTTP complete - clear waiting state BEFORE get_result
            // (get_result may call execute_tool which calls vm_step)
            vm->waiting_for_agent = NULL;
            Value saved_msg = vm->waiting_msg;
            vm->waiting_msg = value_null();

            // Get result (may trigger another async request for tool loop)
            VegaString* response = agent_get_message_result(vm, agent);
            if (response == NULL) {
                // Tool loop in progress - another async request started
                vm->waiting_for_agent = agent;
                vm->waiting_msg = saved_msg;
                return true;
            }
            // Got final result
            value_release(saved_msg);
            vm_push(vm, value_string(response));
            return true;
        } else {
            // Error - poll returned -1
            vm->waiting_for_agent = NULL;
            value_release(vm->waiting_msg);
            vm->waiting_msg = value_null();
            vm_push(vm, value_string(vega_string_from_cstr("Error: Async request failed")));
            return true;
        }
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
                strcmp(name_z, "str::len") == 0 ||
                strcmp(name_z, "str::from_int") == 0 ||
                strcmp(name_z, "str::char_code") == 0 ||
                strcmp(name_z, "str::char_lower") == 0 ||
                strcmp(name_z, "http::get") == 0 ||
                strcmp(name_z, "json::array_len") == 0) {
                argc = 1;
                args[0] = vm_pop(vm);
            } else if (strcmp(name_z, "file::write") == 0 ||
                       strcmp(name_z, "str::contains") == 0 ||
                       strcmp(name_z, "str::char_at") == 0 ||
                       strcmp(name_z, "str::split") == 0 ||
                       strcmp(name_z, "str::split_len") == 0 ||
                       strcmp(name_z, "json::get_string") == 0 ||
                       strcmp(name_z, "json::get_float") == 0 ||
                       strcmp(name_z, "json::get_int") == 0 ||
                       strcmp(name_z, "json::get_array") == 0 ||
                       strcmp(name_z, "json::array_get") == 0) {
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

        case OP_SPAWN_ASYNC: {
            // For now, async spawn works like regular spawn
            // (blocking behavior, but the syntax is supported)
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

        case OP_AWAIT: {
            // Await a future: block until result is ready
            Value future_val = vm_pop(vm);

            if (future_val.type != VAL_FUTURE || !future_val.as.future) {
                snprintf(vm->error_msg, sizeof(vm->error_msg),
                        "Cannot await non-future value");
                vm->had_error = true;
                vm->running = false;
                value_release(future_val);
                break;
            }

            VegaFuture* future = future_val.as.future;

            if (future_is_ready(future)) {
                // Already resolved - push result
                if (future->state == FUTURE_READY) {
                    VegaString* result = future->result;
                    if (result) {
                        vega_obj_retain(result);
                        vm_push(vm, value_string(result));
                    } else {
                        vm_push(vm, value_null());
                    }
                } else {
                    // Error state
                    const char* err = future->error ? future->error : "Unknown error";
                    vm_push(vm, value_string(vega_string_from_cstr(err)));
                }
                vega_obj_release(future);
            } else {
                // Not ready yet - set up blocking wait on the agent
                // Push future back so we can retry
                vm->ip--;  // Replay OP_AWAIT
                vm_push(vm, future_val);  // Put future back on stack
                vm->waiting_for_agent = future->agent;
                // Return true to yield
            }
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
                value_release(msg);
                value_release(target);
                break;
            }

            VegaAgent* agent = target.as.agent;
            VegaString* msg_str = value_to_string(msg);

            // Start async request
            if (agent_start_message_async(vm, agent, msg_str->data)) {
                // Request started - store state and yield
                vm->waiting_for_agent = agent;
                vm->waiting_msg = msg;  // Keep reference for debugging
                vega_obj_release(msg_str);
                value_release(target);
                // Response will be pushed when polling completes
            } else {
                // Failed to start - push error
                value_release(msg);
                vega_obj_release(msg_str);
                value_release(target);
                vm_push(vm, value_string(vega_string_from_cstr("Error: Failed to send message")));
            }
            break;
        }

        case OP_SEND_ASYNC: {
            // Async send: returns a future immediately instead of blocking
            Value msg = vm_pop(vm);
            Value target = vm_pop(vm);

            if (target.type != VAL_AGENT || !target.as.agent) {
                snprintf(vm->error_msg, sizeof(vm->error_msg),
                        "Cannot send message to non-agent");
                vm->had_error = true;
                vm->running = false;
                value_release(msg);
                value_release(target);
                break;
            }

            if (vm->pending_count >= VM_MAX_PENDING) {
                snprintf(vm->error_msg, sizeof(vm->error_msg),
                        "Too many pending async requests (max %d)", VM_MAX_PENDING);
                vm->had_error = true;
                vm->running = false;
                value_release(msg);
                value_release(target);
                break;
            }

            VegaAgent* agent = target.as.agent;
            VegaString* msg_str = value_to_string(msg);

            // Create future for this request
            uint32_t request_id = vm->next_request_id++;
            VegaFuture* future = future_new(agent, request_id);

            // Start async request
            if (agent_start_message_async(vm, agent, msg_str->data)) {
                // Request started - add to pending futures
                vm->pending_futures[vm->pending_count++] = future;
                // Push future onto stack immediately (non-blocking)
                vm_push(vm, value_future(future));
            } else {
                // Failed to start
                future_set_error(future, "Failed to start async request");
                vm_push(vm, value_future(future));
            }

            vega_obj_release(msg_str);
            value_release(msg);
            value_release(target);
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

        case OP_ARRAY_NEW: {
            uint16_t capacity = READ_U16(vm->code, vm->ip);
            vm->ip += 2;
            VegaArray* arr = array_new(capacity > 0 ? capacity : 4);
            vm_push(vm, (Value){.type = VAL_ARRAY, .as.array = arr});
            break;
        }

        case OP_ARRAY_PUSH: {
            Value elem = vm_pop(vm);
            Value arr_val = vm_pop(vm);
            if (arr_val.type == VAL_ARRAY && arr_val.as.array) {
                array_push(arr_val.as.array, elem);
            }
            vm_push(vm, arr_val);  // Put array back on stack
            break;
        }

        case OP_ARRAY_GET: {
            Value idx = vm_pop(vm);
            Value arr_val = vm_pop(vm);
            if (arr_val.type == VAL_ARRAY && arr_val.as.array && idx.type == VAL_INT) {
                Value result = array_get(arr_val.as.array, (uint32_t)idx.as.integer);
                value_retain(result);
                vm_push(vm, result);
            } else {
                vm_push(vm, value_null());
            }
            value_release(arr_val);
            break;
        }

        case OP_ARRAY_SET: {
            Value val = vm_pop(vm);
            Value idx = vm_pop(vm);
            Value arr_val = vm_pop(vm);
            if (arr_val.type == VAL_ARRAY && arr_val.as.array && idx.type == VAL_INT) {
                array_set(arr_val.as.array, (uint32_t)idx.as.integer, val);
            }
            value_release(arr_val);
            break;
        }

        case OP_ARRAY_LEN: {
            Value arr_val = vm_pop(vm);
            if (arr_val.type == VAL_ARRAY && arr_val.as.array) {
                vm_push(vm, value_int(array_length(arr_val.as.array)));
            } else {
                vm_push(vm, value_int(0));
            }
            value_release(arr_val);
            break;
        }

        case OP_PRINT: {
            Value v = vm_pop(vm);
            // If tracing is enabled (TUI mode), route output through trace system
            if (trace_is_enabled()) {
                VegaString* str = value_to_string(v);
                trace_print(str->data);
                vega_obj_release(str);
            } else {
                value_print(v);
                printf("\n");
                fflush(stdout);
            }
            value_release(v);
            // Push null as return value (for expression statement semantics)
            vm_push(vm, value_null());
            break;
        }

        case OP_HALT:
            vm->running = false;
            break;

        case OP_RESULT_OK: {
            Value val = vm_pop(vm);
            Value result = value_result_ok(val);
            vm_push(vm, result);
            break;
        }

        case OP_RESULT_ERR: {
            Value val = vm_pop(vm);
            Value result = value_result_err(val);
            vm_push(vm, result);
            break;
        }

        case OP_RESULT_IS_OK: {
            Value result = vm_pop(vm);
            bool is_ok = (result.type == VAL_RESULT && result.as.result && result.as.result->is_ok);
            vm_push(vm, value_bool(is_ok));
            value_release(result);
            break;
        }

        case OP_RESULT_UNWRAP: {
            Value result = vm_pop(vm);
            if (result.type == VAL_RESULT && result.as.result) {
                Value unwrapped = result.as.result->value;
                value_retain(unwrapped);
                vm_push(vm, unwrapped);
            } else {
                vm_push(vm, value_null());
            }
            value_release(result);
            break;
        }

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
        fprintf(stderr, "Warning: API key not set. Add to ~/.vega or set ANTHROPIC_API_KEY\n");
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
