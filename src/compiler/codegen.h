#ifndef VEGA_CODEGEN_H
#define VEGA_CODEGEN_H

#include "ast.h"
#include "../common/bytecode.h"
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

/*
 * Vega Code Generator
 *
 * Emits bytecode from validated AST.
 */

// ============================================================================
// Bytecode Builder
// ============================================================================

typedef struct {
    uint8_t* code;
    uint32_t code_size;
    uint32_t code_capacity;

    // Constant pool
    uint8_t* constants;
    uint32_t const_size;
    uint32_t const_capacity;

    // String interning (for deduplication)
    char** strings;
    uint16_t* string_indices;
    uint32_t string_count;
    uint32_t string_capacity;

    // Function table
    FunctionDef* functions;
    uint32_t func_count;
    uint32_t func_capacity;

    // Agent definitions
    AgentDef* agents;
    uint32_t agent_count;
    uint32_t agent_capacity;

    // Local variable slots for current function
    char** locals;
    uint32_t local_count;
    uint32_t local_capacity;

    // Loop jump tracking (for break/continue)
    uint32_t* loop_starts;      // Stack of loop start offsets (for continue)
    uint32_t* break_patches;    // Offsets that need patching for break
    uint32_t break_count;
    uint32_t loop_depth;
    uint32_t loop_capacity;

    // Error tracking
    bool had_error;
    char error_msg[256];
} CodeGen;

// ============================================================================
// API
// ============================================================================

// Initialize code generator
void codegen_init(CodeGen* cg);

// Cleanup code generator
void codegen_cleanup(CodeGen* cg);

// Generate bytecode from program
bool codegen_generate(CodeGen* cg, AstProgram* program);

// Write bytecode to file
bool codegen_write_file(CodeGen* cg, const char* filename);

// Write human-readable disassembly
void codegen_disassemble(CodeGen* cg, FILE* out);

// Error handling
bool codegen_had_error(CodeGen* cg);
const char* codegen_error_msg(CodeGen* cg);

#endif // VEGA_CODEGEN_H
