#ifndef VEGA_VM_H
#define VEGA_VM_H

#include "value.h"
#include "process.h"
#include "scheduler.h"
#include "../common/bytecode.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Vega Virtual Machine
 *
 * Stack-based bytecode interpreter.
 */

// ============================================================================
// Constants
// ============================================================================

#define VM_STACK_MAX    256
#define VM_FRAMES_MAX   64
#define VM_GLOBALS_MAX  256

// ============================================================================
// Call Frame
// ============================================================================

typedef struct {
    uint32_t function_id;
    uint32_t ip;            // Instruction pointer (return address)
    uint32_t bp;            // Base pointer (stack frame start)
} CallFrame;

// ============================================================================
// VM State
// ============================================================================

typedef struct VegaVM {
    // Bytecode
    uint8_t* code;
    uint32_t code_size;

    // Constant pool
    uint8_t* constants;
    uint32_t const_size;

    // Function table
    FunctionDef* functions;
    uint32_t func_count;

    // Agent definitions
    AgentDef* agents;
    uint32_t agent_count;

    // Execution state
    uint32_t ip;            // Instruction pointer
    Value stack[VM_STACK_MAX];
    uint32_t sp;            // Stack pointer

    // Call stack
    CallFrame frames[VM_FRAMES_MAX];
    uint32_t frame_count;

    // Globals
    Value globals[VM_GLOBALS_MAX];
    char* global_names[VM_GLOBALS_MAX];
    uint32_t global_count;

    // Locals for current frame
    Value* locals;
    uint32_t local_count;

    // Runtime state
    bool running;
    bool had_error;
    char error_msg[256];

    // API key (from environment or ~/.vega config)
    char* api_key;

    // Process model (Phase 2)
    VegaProcess* processes[MAX_PROCESSES];
    uint32_t process_count;
    uint32_t next_pid;
    Scheduler scheduler;
} VegaVM;

// ============================================================================
// VM API
// ============================================================================

// Initialize VM
void vm_init(VegaVM* vm);

// Free VM resources
void vm_free(VegaVM* vm);

// Load bytecode from file
bool vm_load_file(VegaVM* vm, const char* filename);

// Load bytecode from memory
bool vm_load(VegaVM* vm, uint8_t* bytecode, uint32_t size);

// Run the program (calls main)
bool vm_run(VegaVM* vm);

// Execute single instruction (for debugging)
bool vm_step(VegaVM* vm);

// Error handling
bool vm_had_error(VegaVM* vm);
const char* vm_error_msg(VegaVM* vm);

// Stack operations
void vm_push(VegaVM* vm, Value v);
Value vm_pop(VegaVM* vm);
Value vm_peek(VegaVM* vm, uint32_t distance);

// Constant pool access
Value vm_read_constant(VegaVM* vm, uint16_t index);
const char* vm_read_string(VegaVM* vm, uint16_t index, uint32_t* out_len);
const char* vm_find_string_after_key(VegaVM* vm, const char* key, uint32_t* out_len);

// Global variable access
Value vm_get_global(VegaVM* vm, const char* name);
void vm_set_global(VegaVM* vm, const char* name, Value v);

// Function lookup
int vm_find_function(VegaVM* vm, const char* name);

// Agent lookup
int vm_find_agent(VegaVM* vm, const char* name);
AgentDef* vm_get_agent(VegaVM* vm, uint32_t index);

// Debug
void vm_print_stack(VegaVM* vm);
void vm_disassemble_instruction(VegaVM* vm, uint32_t offset);

// Process execution (called by scheduler)
void vm_execute_process(VegaVM* vm, VegaProcess* proc);

#endif // VEGA_VM_H
