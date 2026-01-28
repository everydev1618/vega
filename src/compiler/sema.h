#ifndef VEGA_SEMA_H
#define VEGA_SEMA_H

#include "ast.h"
#include <stdbool.h>

/*
 * Vega Semantic Analyzer
 *
 * Performs:
 * - Symbol table management
 * - Type checking
 * - Agent reference validation
 * - Scope resolution
 */

// ============================================================================
// Type System
// ============================================================================

typedef enum {
    TYPE_VOID,
    TYPE_INT,
    TYPE_FLOAT,
    TYPE_BOOL,
    TYPE_STRING,
    TYPE_AGENT,         // Agent handle
    TYPE_FUTURE,        // Async agent result
    TYPE_RESULT,        // Result type for error handling
    TYPE_ARRAY,         // Array of another type
    TYPE_UNKNOWN,       // For error recovery
} VegaType;

typedef struct {
    VegaType kind;
    VegaType element_type;  // For arrays
    char* agent_name;       // For agent handles
} TypeInfo;

// ============================================================================
// Symbol Table
// ============================================================================

typedef enum {
    SYM_VARIABLE,
    SYM_FUNCTION,
    SYM_AGENT,
    SYM_PARAMETER,
    SYM_TOOL,
} SymbolKind;

typedef struct Symbol {
    char* name;
    SymbolKind kind;
    TypeInfo type;
    SourceLoc defined_at;

    // For functions/tools
    TypeInfo* param_types;
    uint32_t param_count;
    TypeInfo return_type;

    // For agents
    char** tool_names;
    uint32_t tool_count;

    struct Symbol* next;  // For hash chain
} Symbol;

typedef struct Scope {
    Symbol** symbols;       // Hash table
    uint32_t capacity;
    struct Scope* parent;
} Scope;

// ============================================================================
// Semantic Analyzer State
// ============================================================================

typedef struct {
    Scope* global_scope;
    Scope* current_scope;

    // Error tracking
    bool had_error;
    char error_msg[512];
    SourceLoc error_loc;

    // Context
    AstDecl* current_function;  // Current function being analyzed
    AstDecl* current_agent;     // Current agent being analyzed
    bool in_loop;               // For break/continue validation
} SemanticAnalyzer;

// ============================================================================
// API
// ============================================================================

// Initialize/cleanup
void sema_init(SemanticAnalyzer* sema);
void sema_cleanup(SemanticAnalyzer* sema);

// Analyze program
bool sema_analyze(SemanticAnalyzer* sema, AstProgram* program);

// Error reporting
bool sema_had_error(SemanticAnalyzer* sema);
const char* sema_error_msg(SemanticAnalyzer* sema);
SourceLoc sema_error_loc(SemanticAnalyzer* sema);

// Type utilities
const char* type_name(VegaType type);
bool types_equal(TypeInfo* a, TypeInfo* b);
TypeInfo type_from_annotation(TypeAnnotation* annotation);

#endif // VEGA_SEMA_H
