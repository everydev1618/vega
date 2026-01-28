#include "sema.h"
#include "parser.h"
#include "lexer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <libgen.h>
#include <sys/stat.h>
#include <limits.h>

// ============================================================================
// Hash Function
// ============================================================================

static uint32_t hash_string(const char* str) {
    uint32_t hash = 2166136261u;
    while (*str) {
        hash ^= (uint8_t)*str++;
        hash *= 16777619u;
    }
    return hash;
}

// ============================================================================
// Scope Management
// ============================================================================

static Scope* scope_new(Scope* parent) {
    Scope* scope = malloc(sizeof(Scope));
    scope->capacity = 64;
    scope->symbols = calloc(scope->capacity, sizeof(Symbol*));
    scope->parent = parent;
    return scope;
}

static void scope_free(Scope* scope) {
    if (!scope) return;

    for (uint32_t i = 0; i < scope->capacity; i++) {
        Symbol* sym = scope->symbols[i];
        while (sym) {
            Symbol* next = sym->next;
            free(sym->name);
            free(sym->param_types);
            free(sym->type.agent_name);
            if (sym->tool_names) {
                for (uint32_t j = 0; j < sym->tool_count; j++) {
                    free(sym->tool_names[j]);
                }
                free(sym->tool_names);
            }
            free(sym);
            sym = next;
        }
    }

    free(scope->symbols);
    free(scope);
}

static void scope_add(Scope* scope, Symbol* symbol) {
    uint32_t idx = hash_string(symbol->name) % scope->capacity;
    symbol->next = scope->symbols[idx];
    scope->symbols[idx] = symbol;
}

static Symbol* scope_lookup_local(Scope* scope, const char* name) {
    uint32_t idx = hash_string(name) % scope->capacity;
    Symbol* sym = scope->symbols[idx];
    while (sym) {
        if (strcmp(sym->name, name) == 0) {
            return sym;
        }
        sym = sym->next;
    }
    return NULL;
}

static Symbol* scope_lookup(Scope* scope, const char* name) {
    while (scope) {
        Symbol* sym = scope_lookup_local(scope, name);
        if (sym) return sym;
        scope = scope->parent;
    }
    return NULL;
}

// Forward declarations
static void sema_error(SemanticAnalyzer* sema, SourceLoc loc, const char* fmt, ...);
TypeInfo type_from_annotation(TypeAnnotation* annotation);  // Declared in header

// ============================================================================
// Module Cache
// ============================================================================

static void module_cache_init(ModuleCache* cache) {
    cache->capacity = 64;
    cache->modules = calloc(cache->capacity, sizeof(Module*));
    cache->search_path_count = 0;
}

static void module_cache_free(ModuleCache* cache) {
    for (uint32_t i = 0; i < cache->capacity; i++) {
        Module* mod = cache->modules[i];
        while (mod) {
            Module* next = mod->next;
            free(mod->path);
            free(mod->source);
            if (mod->ast) ast_program_free(mod->ast);
            free(mod);
            mod = next;
        }
    }
    free(cache->modules);
    for (uint32_t i = 0; i < cache->search_path_count; i++) {
        free(cache->search_paths[i]);
    }
}

static Module* module_cache_get(ModuleCache* cache, const char* path) {
    uint32_t idx = hash_string(path) % cache->capacity;
    Module* mod = cache->modules[idx];
    while (mod) {
        if (strcmp(mod->path, path) == 0) return mod;
        mod = mod->next;
    }
    return NULL;
}

static void module_cache_add(ModuleCache* cache, Module* mod) {
    uint32_t idx = hash_string(mod->path) % cache->capacity;
    mod->next = cache->modules[idx];
    cache->modules[idx] = mod;
}

// ============================================================================
// Path Resolution
// ============================================================================

static bool file_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static char* read_file_contents(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* content = malloc(size + 1);
    if (!content) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(content, 1, size, f);
    content[read] = '\0';
    fclose(f);
    return content;
}

// Resolve import path to actual file path
// Returns allocated string or NULL if not found
static char* resolve_import_path(SemanticAnalyzer* sema, const char* import_path) {
    char resolved[PATH_MAX];
    char* result = NULL;

    // Relative import: "./foo" or "../foo"
    if (import_path[0] == '.') {
        // Get directory of current file
        char* current_copy = strdup(sema->current_file);
        char* dir = dirname(current_copy);

        snprintf(resolved, sizeof(resolved), "%s/%s.vega", dir, import_path);
        free(current_copy);

        if (file_exists(resolved)) {
            result = realpath(resolved, NULL);
            if (result) return result;
        }
    }

    // Search in search paths (stdlib, etc.)
    for (uint32_t i = 0; i < sema->modules.search_path_count; i++) {
        snprintf(resolved, sizeof(resolved), "%s/%s.vega",
                 sema->modules.search_paths[i], import_path);
        if (file_exists(resolved)) {
            result = realpath(resolved, NULL);
            if (result) return result;
        }
    }

    return NULL;
}

// Forward declaration
static bool process_module(SemanticAnalyzer* sema, const char* path);

// Process a single import declaration
static bool process_import(SemanticAnalyzer* sema, ImportDecl* import, SourceLoc loc) {
    // Resolve the import path
    char* resolved = resolve_import_path(sema, import->path);
    if (!resolved) {
        sema_error(sema, loc, "Cannot find module '%s'", import->path);
        return false;
    }

    // Check if already in cache
    Module* mod = module_cache_get(&sema->modules, resolved);
    if (mod) {
        if (mod->analyzing) {
            sema_error(sema, loc, "Circular import detected: %s", import->path);
            free(resolved);
            return false;
        }
        // Already loaded, symbols are already in global scope
        free(resolved);
        return true;
    }

    // Load and process the module
    if (!process_module(sema, resolved)) {
        free(resolved);
        return false;
    }

    free(resolved);
    return true;
}

// Load, parse, and analyze a module
static bool process_module(SemanticAnalyzer* sema, const char* path) {
    // Read source
    char* source = read_file_contents(path);
    if (!source) {
        fprintf(stderr, "Error: Cannot read module '%s'\n", path);
        return false;
    }

    // Create module entry
    Module* mod = malloc(sizeof(Module));
    mod->path = strdup(path);
    mod->source = source;
    mod->ast = NULL;
    mod->analyzing = true;
    mod->analyzed = false;
    mod->next = NULL;
    module_cache_add(&sema->modules, mod);

    // Parse
    Lexer lexer;
    lexer_init(&lexer, source, path);

    Parser parser;
    parser_init(&parser, &lexer);

    AstProgram* ast = parser_parse_program(&parser);
    if (parser_had_error(&parser)) {
        mod->analyzing = false;
        return false;
    }
    mod->ast = ast;

    // Save current file context
    const char* saved_file = sema->current_file;
    sema->current_file = path;

    // Process imports in this module first
    for (uint32_t i = 0; i < ast->decl_count; i++) {
        if (ast->decls[i]->kind == DECL_IMPORT) {
            if (!process_import(sema, &ast->decls[i]->as.import, ast->decls[i]->loc)) {
                sema->current_file = saved_file;
                mod->analyzing = false;
                return false;
            }
        }
    }

    // Register declarations from this module into global scope
    for (uint32_t i = 0; i < ast->decl_count; i++) {
        AstDecl* decl = ast->decls[i];

        if (decl->kind == DECL_AGENT) {
            Symbol* sym = malloc(sizeof(Symbol));
            sym->name = strdup(decl->as.agent.name);
            sym->kind = SYM_AGENT;
            sym->type = (TypeInfo){.kind = TYPE_AGENT,
                                   .agent_name = strdup(decl->as.agent.name)};
            sym->defined_at = decl->loc;
            sym->tool_count = decl->as.agent.tool_count;
            sym->tool_names = malloc(sym->tool_count * sizeof(char*));
            for (uint32_t j = 0; j < sym->tool_count; j++) {
                sym->tool_names[j] = strdup(decl->as.agent.tools[j].name);
            }
            sym->param_types = NULL;
            sym->param_count = 0;
            sym->next = NULL;
            scope_add(sema->global_scope, sym);
        }
        else if (decl->kind == DECL_FUNCTION) {
            Symbol* sym = malloc(sizeof(Symbol));
            sym->name = strdup(decl->as.function.name);
            sym->kind = SYM_FUNCTION;
            sym->type = (TypeInfo){.kind = TYPE_VOID};
            sym->return_type = type_from_annotation(&decl->as.function.return_type);
            sym->param_count = decl->as.function.param_count;
            sym->param_types = malloc(sym->param_count * sizeof(TypeInfo));
            for (uint32_t j = 0; j < sym->param_count; j++) {
                sym->param_types[j] = type_from_annotation(&decl->as.function.params[j].type);
            }
            sym->defined_at = decl->loc;
            sym->tool_names = NULL;
            sym->tool_count = 0;
            sym->next = NULL;
            scope_add(sema->global_scope, sym);
        }
    }

    // Restore context
    sema->current_file = saved_file;
    mod->analyzing = false;
    mod->analyzed = true;

    return true;
}

// ============================================================================
// Error Reporting
// ============================================================================

static void sema_error(SemanticAnalyzer* sema, SourceLoc loc, const char* fmt, ...) {
    if (sema->had_error) return;  // Only report first error

    sema->had_error = true;
    sema->error_loc = loc;

    va_list args;
    va_start(args, fmt);
    vsnprintf(sema->error_msg, sizeof(sema->error_msg), fmt, args);
    va_end(args);

    fprintf(stderr, "%s:%u:%u: error: %s\n",
            loc.filename, loc.line, loc.column, sema->error_msg);
}

// ============================================================================
// Type Utilities
// ============================================================================

const char* type_name(VegaType type) {
    switch (type) {
        case TYPE_VOID:    return "void";
        case TYPE_INT:     return "int";
        case TYPE_FLOAT:   return "float";
        case TYPE_BOOL:    return "bool";
        case TYPE_STRING:  return "str";
        case TYPE_AGENT:   return "agent";
        case TYPE_FUTURE:  return "future";
        case TYPE_RESULT:  return "result";
        case TYPE_ARRAY:   return "array";
        case TYPE_UNKNOWN: return "unknown";
        default:           return "?";
    }
}

bool types_equal(TypeInfo* a, TypeInfo* b) {
    if (a->kind != b->kind) return false;
    if (a->kind == TYPE_ARRAY) {
        // Allow unknown element type to match any array
        if (a->element_type == TYPE_UNKNOWN || b->element_type == TYPE_UNKNOWN) {
            return true;
        }
        return a->element_type == b->element_type;
    }
    if (a->kind == TYPE_AGENT) {
        if (!a->agent_name || !b->agent_name) return true;  // Generic agent
        return strcmp(a->agent_name, b->agent_name) == 0;
    }
    return true;
}

TypeInfo type_from_annotation(TypeAnnotation* annotation) {
    TypeInfo info = {0};

    if (!annotation || !annotation->name) {
        info.kind = TYPE_VOID;
        return info;
    }

    if (strcmp(annotation->name, "int") == 0) {
        info.kind = TYPE_INT;
    } else if (strcmp(annotation->name, "float") == 0) {
        info.kind = TYPE_FLOAT;
    } else if (strcmp(annotation->name, "bool") == 0) {
        info.kind = TYPE_BOOL;
    } else if (strcmp(annotation->name, "str") == 0) {
        info.kind = TYPE_STRING;
    } else if (strcmp(annotation->name, "void") == 0) {
        info.kind = TYPE_VOID;
    } else if (strcmp(annotation->name, "Result") == 0 || annotation->is_result) {
        info.kind = TYPE_RESULT;
    } else {
        // Could be an agent type
        info.kind = TYPE_AGENT;
        info.agent_name = strdup(annotation->name);
    }

    if (annotation->is_array) {
        info.element_type = info.kind;
        info.kind = TYPE_ARRAY;
    }

    return info;
}

// ============================================================================
// Expression Type Analysis
// ============================================================================

static TypeInfo analyze_expr(SemanticAnalyzer* sema, AstExpr* expr);

static TypeInfo analyze_binary(SemanticAnalyzer* sema, AstExpr* expr) {
    TypeInfo left = analyze_expr(sema, expr->as.binary.left);
    TypeInfo right = analyze_expr(sema, expr->as.binary.right);

    switch (expr->as.binary.op) {
        case BINOP_ADD:
            // Array concatenation
            if (left.kind == TYPE_ARRAY && right.kind == TYPE_ARRAY) {
                // Use the element type from the left operand, or right if left is unknown
                VegaType elem_type = left.element_type != TYPE_UNKNOWN ?
                    left.element_type : right.element_type;
                return (TypeInfo){.kind = TYPE_ARRAY, .element_type = elem_type};
            }
            // String concatenation
            if (left.kind == TYPE_STRING || right.kind == TYPE_STRING) {
                return (TypeInfo){.kind = TYPE_STRING};
            }
            // Numeric addition
            if (left.kind == TYPE_INT && right.kind == TYPE_INT) {
                return (TypeInfo){.kind = TYPE_INT};
            }
            if ((left.kind == TYPE_INT || left.kind == TYPE_FLOAT) &&
                (right.kind == TYPE_INT || right.kind == TYPE_FLOAT)) {
                return (TypeInfo){.kind = TYPE_FLOAT};
            }
            sema_error(sema, expr->loc, "Cannot add %s and %s",
                      type_name(left.kind), type_name(right.kind));
            break;

        case BINOP_SUB:
        case BINOP_MUL:
        case BINOP_DIV:
        case BINOP_MOD:
            if (left.kind == TYPE_INT && right.kind == TYPE_INT) {
                return (TypeInfo){.kind = TYPE_INT};
            }
            if ((left.kind == TYPE_INT || left.kind == TYPE_FLOAT) &&
                (right.kind == TYPE_INT || right.kind == TYPE_FLOAT)) {
                return (TypeInfo){.kind = TYPE_FLOAT};
            }
            sema_error(sema, expr->loc, "Arithmetic requires numeric types");
            break;

        case BINOP_EQ:
        case BINOP_NE:
        case BINOP_LT:
        case BINOP_LE:
        case BINOP_GT:
        case BINOP_GE:
            return (TypeInfo){.kind = TYPE_BOOL};

        case BINOP_AND:
        case BINOP_OR:
            if (left.kind != TYPE_BOOL) {
                sema_error(sema, expr->as.binary.left->loc,
                          "Logical operator requires bool, got %s",
                          type_name(left.kind));
            }
            if (right.kind != TYPE_BOOL) {
                sema_error(sema, expr->as.binary.right->loc,
                          "Logical operator requires bool, got %s",
                          type_name(right.kind));
            }
            return (TypeInfo){.kind = TYPE_BOOL};
    }

    return (TypeInfo){.kind = TYPE_UNKNOWN};
}

static TypeInfo analyze_expr(SemanticAnalyzer* sema, AstExpr* expr) {
    if (!expr) return (TypeInfo){.kind = TYPE_UNKNOWN};

    switch (expr->kind) {
        case EXPR_INT_LITERAL:
            return (TypeInfo){.kind = TYPE_INT};

        case EXPR_FLOAT_LITERAL:
            return (TypeInfo){.kind = TYPE_FLOAT};

        case EXPR_STRING_LITERAL:
            return (TypeInfo){.kind = TYPE_STRING};

        case EXPR_BOOL_LITERAL:
            return (TypeInfo){.kind = TYPE_BOOL};

        case EXPR_NULL_LITERAL:
            return (TypeInfo){.kind = TYPE_UNKNOWN};  // Null is polymorphic

        case EXPR_IDENTIFIER: {
            Symbol* sym = scope_lookup(sema->current_scope, expr->as.ident.name);
            if (!sym) {
                sema_error(sema, expr->loc, "Undefined variable '%s'",
                          expr->as.ident.name);
                return (TypeInfo){.kind = TYPE_UNKNOWN};
            }
            return sym->type;
        }

        case EXPR_BINARY:
            return analyze_binary(sema, expr);

        case EXPR_UNARY:
            if (expr->as.unary.op == UNOP_NOT) {
                TypeInfo operand = analyze_expr(sema, expr->as.unary.operand);
                if (operand.kind != TYPE_BOOL) {
                    sema_error(sema, expr->loc, "! operator requires bool");
                }
                return (TypeInfo){.kind = TYPE_BOOL};
            } else {
                TypeInfo operand = analyze_expr(sema, expr->as.unary.operand);
                if (operand.kind != TYPE_INT && operand.kind != TYPE_FLOAT) {
                    sema_error(sema, expr->loc, "Unary - requires numeric type");
                }
                return operand;
            }

        case EXPR_CALL: {
            // Check callee
            if (expr->as.call.callee->kind == EXPR_IDENTIFIER) {
                const char* name = expr->as.call.callee->as.ident.name;

                // Check for built-in functions
                if (strcmp(name, "print") == 0) {
                    // print accepts anything
                    for (uint32_t i = 0; i < expr->as.call.arg_count; i++) {
                        analyze_expr(sema, expr->as.call.args[i]);
                    }
                    return (TypeInfo){.kind = TYPE_VOID};
                }

                // Check for module::function calls
                if (strstr(name, "::") != NULL) {
                    // stdlib call - analyze args and return appropriate type
                    for (uint32_t i = 0; i < expr->as.call.arg_count; i++) {
                        analyze_expr(sema, expr->as.call.args[i]);
                    }
                    if (strncmp(name, "file::", 6) == 0) {
                        if (strstr(name, "read")) return (TypeInfo){.kind = TYPE_STRING};
                        if (strstr(name, "exists")) return (TypeInfo){.kind = TYPE_BOOL};
                        return (TypeInfo){.kind = TYPE_VOID};
                    }
                    if (strncmp(name, "str::", 5) == 0) {
                        if (strstr(name, "len")) return (TypeInfo){.kind = TYPE_INT};
                        if (strstr(name, "char_code")) return (TypeInfo){.kind = TYPE_INT};
                        if (strstr(name, "split_len")) return (TypeInfo){.kind = TYPE_INT};
                        if (strstr(name, "contains")) return (TypeInfo){.kind = TYPE_BOOL};
                        if (strstr(name, "split")) return (TypeInfo){.kind = TYPE_ARRAY, .element_type = TYPE_STRING};
                        return (TypeInfo){.kind = TYPE_STRING};
                    }
                    if (strncmp(name, "json::", 6) == 0) {
                        if (strstr(name, "get_string")) return (TypeInfo){.kind = TYPE_STRING};
                        if (strstr(name, "get_float")) return (TypeInfo){.kind = TYPE_FLOAT};
                        if (strstr(name, "get_int")) return (TypeInfo){.kind = TYPE_INT};
                        if (strstr(name, "get_array")) return (TypeInfo){.kind = TYPE_STRING};  // JSON array as string
                        if (strstr(name, "array_len")) return (TypeInfo){.kind = TYPE_INT};
                        if (strstr(name, "array_get")) return (TypeInfo){.kind = TYPE_STRING};
                        return (TypeInfo){.kind = TYPE_STRING};
                    }
                    if (strncmp(name, "http::", 6) == 0) {
                        return (TypeInfo){.kind = TYPE_STRING};  // HTTP functions return strings
                    }
                    return (TypeInfo){.kind = TYPE_UNKNOWN};
                }

                Symbol* sym = scope_lookup(sema->current_scope, name);
                if (!sym) {
                    sema_error(sema, expr->loc, "Undefined function '%s'", name);
                    return (TypeInfo){.kind = TYPE_UNKNOWN};
                }
                if (sym->kind != SYM_FUNCTION && sym->kind != SYM_TOOL) {
                    sema_error(sema, expr->loc, "'%s' is not a function", name);
                    return (TypeInfo){.kind = TYPE_UNKNOWN};
                }

                // Check argument count
                if (expr->as.call.arg_count != sym->param_count) {
                    sema_error(sema, expr->loc,
                              "Function '%s' expects %u arguments, got %u",
                              name, sym->param_count, expr->as.call.arg_count);
                }

                // Analyze arguments
                for (uint32_t i = 0; i < expr->as.call.arg_count; i++) {
                    analyze_expr(sema, expr->as.call.args[i]);
                }

                return sym->return_type;
            }

            // Generic call - analyze callee and args
            analyze_expr(sema, expr->as.call.callee);
            for (uint32_t i = 0; i < expr->as.call.arg_count; i++) {
                analyze_expr(sema, expr->as.call.args[i]);
            }
            return (TypeInfo){.kind = TYPE_UNKNOWN};
        }

        case EXPR_METHOD_CALL: {
            TypeInfo obj_type = analyze_expr(sema, expr->as.method_call.object);

            // String methods
            if (obj_type.kind == TYPE_STRING) {
                if (strcmp(expr->as.method_call.method, "has") == 0 ||
                    strcmp(expr->as.method_call.method, "contains") == 0) {
                    return (TypeInfo){.kind = TYPE_BOOL};
                }
                if (strcmp(expr->as.method_call.method, "len") == 0) {
                    return (TypeInfo){.kind = TYPE_INT};
                }
            }

            // Analyze arguments
            for (uint32_t i = 0; i < expr->as.method_call.arg_count; i++) {
                analyze_expr(sema, expr->as.method_call.args[i]);
            }

            return (TypeInfo){.kind = TYPE_UNKNOWN};
        }

        case EXPR_FIELD_ACCESS: {
            analyze_expr(sema, expr->as.field_access.object);
            return (TypeInfo){.kind = TYPE_UNKNOWN};
        }

        case EXPR_SPAWN: {
            Symbol* agent = scope_lookup(sema->current_scope, expr->as.spawn.agent_name);
            if (!agent) {
                sema_error(sema, expr->loc, "Undefined agent '%s'",
                          expr->as.spawn.agent_name);
                return (TypeInfo){.kind = TYPE_UNKNOWN};
            }
            if (agent->kind != SYM_AGENT) {
                sema_error(sema, expr->loc, "'%s' is not an agent",
                          expr->as.spawn.agent_name);
                return (TypeInfo){.kind = TYPE_UNKNOWN};
            }

            // Both sync and async spawn return agent handles
            // (async just affects how message sends behave at runtime)
            return (TypeInfo){.kind = TYPE_AGENT,
                              .agent_name = strdup(expr->as.spawn.agent_name)};
        }

        case EXPR_MESSAGE: {
            TypeInfo target = analyze_expr(sema, expr->as.message.target);
            if (target.kind != TYPE_AGENT) {
                sema_error(sema, expr->as.message.target->loc,
                          "Message target must be an agent handle, got %s",
                          type_name(target.kind));
            }
            analyze_expr(sema, expr->as.message.message);
            return (TypeInfo){.kind = TYPE_STRING};  // Responses are strings
        }

        case EXPR_AWAIT: {
            TypeInfo future = analyze_expr(sema, expr->as.await.future);
            // Accept both futures and strings (strings are already-resolved values)
            if (future.kind != TYPE_FUTURE && future.kind != TYPE_STRING) {
                sema_error(sema, expr->loc, "Can only await futures or strings");
            }
            return (TypeInfo){.kind = TYPE_STRING};
        }

        case EXPR_ARRAY_LITERAL: {
            // Analyze all elements and infer element type from first
            TypeInfo elem_type = {.kind = TYPE_UNKNOWN};
            for (uint32_t i = 0; i < expr->as.array_literal.count; i++) {
                TypeInfo t = analyze_expr(sema, expr->as.array_literal.elements[i]);
                if (i == 0) {
                    elem_type = t;
                }
            }
            return (TypeInfo){.kind = TYPE_ARRAY, .element_type = elem_type.kind};
        }

        case EXPR_INDEX: {
            TypeInfo obj = analyze_expr(sema, expr->as.index.object);
            TypeInfo idx = analyze_expr(sema, expr->as.index.index);

            if (idx.kind != TYPE_INT && idx.kind != TYPE_UNKNOWN) {
                sema_error(sema, expr->as.index.index->loc,
                          "Array index must be int, got %s", type_name(idx.kind));
            }

            if (obj.kind == TYPE_ARRAY) {
                // Return the element type
                return (TypeInfo){.kind = obj.element_type};
            }
            if (obj.kind == TYPE_STRING) {
                // String indexing returns string (single char)
                return (TypeInfo){.kind = TYPE_STRING};
            }
            return (TypeInfo){.kind = TYPE_UNKNOWN};
        }

        case EXPR_OK: {
            analyze_expr(sema, expr->as.result_val.value);
            return (TypeInfo){.kind = TYPE_RESULT};
        }

        case EXPR_ERR: {
            analyze_expr(sema, expr->as.result_val.value);
            return (TypeInfo){.kind = TYPE_RESULT};
        }

        case EXPR_MATCH: {
            analyze_expr(sema, expr->as.match.scrutinee);
            // Analyze each arm's body expression with the binding in scope
            for (uint32_t i = 0; i < expr->as.match.arm_count; i++) {
                // Create a scope for the arm binding
                Scope* arm_scope = scope_new(sema->current_scope);
                sema->current_scope = arm_scope;

                // Add the binding variable to scope
                if (expr->as.match.arms[i].binding_name) {
                    Symbol* binding = calloc(1, sizeof(Symbol));
                    binding->name = strdup(expr->as.match.arms[i].binding_name);
                    // Type is unknown (could be the unwrapped Ok or Err value)
                    binding->type = (TypeInfo){.kind = TYPE_UNKNOWN};
                    binding->kind = SYM_VARIABLE;
                    scope_add(arm_scope, binding);
                }

                if (expr->as.match.arms[i].body) {
                    analyze_expr(sema, expr->as.match.arms[i].body);
                }

                // Pop the arm scope
                sema->current_scope = arm_scope->parent;
                scope_free(arm_scope);
            }
            // Match expressions return void when used as statements
            return (TypeInfo){.kind = TYPE_VOID};
        }

        default:
            return (TypeInfo){.kind = TYPE_UNKNOWN};
    }
}

// ============================================================================
// Statement Analysis
// ============================================================================

static void analyze_stmt(SemanticAnalyzer* sema, AstStmt* stmt);

static void analyze_block(SemanticAnalyzer* sema, AstStmt* block) {
    if (!block || block->kind != STMT_BLOCK) return;

    // Create new scope
    Scope* scope = scope_new(sema->current_scope);
    sema->current_scope = scope;

    for (uint32_t i = 0; i < block->as.block.stmt_count; i++) {
        analyze_stmt(sema, block->as.block.stmts[i]);
    }

    // Pop scope
    sema->current_scope = scope->parent;
    scope_free(scope);
}

static void analyze_stmt(SemanticAnalyzer* sema, AstStmt* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case STMT_EXPR:
            analyze_expr(sema, stmt->as.expr.expr);
            break;

        case STMT_LET: {
            // Check for redefinition in same scope
            if (scope_lookup_local(sema->current_scope, stmt->as.let.name)) {
                sema_error(sema, stmt->loc, "Variable '%s' already defined in this scope",
                          stmt->as.let.name);
                return;
            }

            TypeInfo type;
            if (stmt->as.let.type) {
                type = type_from_annotation(stmt->as.let.type);
            } else if (stmt->as.let.init) {
                type = analyze_expr(sema, stmt->as.let.init);
            } else {
                sema_error(sema, stmt->loc,
                          "Variable '%s' needs type annotation or initializer",
                          stmt->as.let.name);
                return;
            }

            if (stmt->as.let.init) {
                TypeInfo init_type = analyze_expr(sema, stmt->as.let.init);
                if (stmt->as.let.type && !types_equal(&type, &init_type) &&
                    init_type.kind != TYPE_UNKNOWN) {
                    sema_error(sema, stmt->loc,
                              "Type mismatch: expected %s, got %s",
                              type_name(type.kind), type_name(init_type.kind));
                }
            }

            Symbol* sym = malloc(sizeof(Symbol));
            sym->name = strdup(stmt->as.let.name);
            sym->kind = SYM_VARIABLE;
            sym->type = type;
            sym->defined_at = stmt->loc;
            sym->next = NULL;
            scope_add(sema->current_scope, sym);
            break;
        }

        case STMT_ASSIGN: {
            TypeInfo target = analyze_expr(sema, stmt->as.assign.target);
            TypeInfo value = analyze_expr(sema, stmt->as.assign.value);
            if (!types_equal(&target, &value) &&
                target.kind != TYPE_UNKNOWN && value.kind != TYPE_UNKNOWN) {
                sema_error(sema, stmt->loc,
                          "Cannot assign %s to %s",
                          type_name(value.kind), type_name(target.kind));
            }
            break;
        }

        case STMT_IF: {
            TypeInfo cond = analyze_expr(sema, stmt->as.if_stmt.condition);
            if (cond.kind != TYPE_BOOL && cond.kind != TYPE_UNKNOWN) {
                sema_error(sema, stmt->as.if_stmt.condition->loc,
                          "Condition must be bool, got %s", type_name(cond.kind));
            }
            analyze_block(sema, stmt->as.if_stmt.then_branch);
            if (stmt->as.if_stmt.else_branch) {
                if (stmt->as.if_stmt.else_branch->kind == STMT_BLOCK) {
                    analyze_block(sema, stmt->as.if_stmt.else_branch);
                } else {
                    analyze_stmt(sema, stmt->as.if_stmt.else_branch);
                }
            }
            break;
        }

        case STMT_WHILE: {
            TypeInfo cond = analyze_expr(sema, stmt->as.while_stmt.condition);
            if (cond.kind != TYPE_BOOL && cond.kind != TYPE_UNKNOWN) {
                sema_error(sema, stmt->as.while_stmt.condition->loc,
                          "Condition must be bool, got %s", type_name(cond.kind));
            }
            bool was_in_loop = sema->in_loop;
            sema->in_loop = true;
            analyze_block(sema, stmt->as.while_stmt.body);
            sema->in_loop = was_in_loop;
            break;
        }

        case STMT_RETURN: {
            if (!sema->current_function) {
                sema_error(sema, stmt->loc, "Return outside of function");
                return;
            }

            TypeInfo expected;
            if (sema->current_function->kind == DECL_FUNCTION) {
                expected = type_from_annotation(&sema->current_function->as.function.return_type);
            } else {
                expected = (TypeInfo){.kind = TYPE_VOID};
            }

            if (stmt->as.return_stmt.value) {
                TypeInfo actual = analyze_expr(sema, stmt->as.return_stmt.value);
                if (!types_equal(&expected, &actual) &&
                    actual.kind != TYPE_UNKNOWN && expected.kind != TYPE_VOID) {
                    sema_error(sema, stmt->loc,
                              "Return type mismatch: expected %s, got %s",
                              type_name(expected.kind), type_name(actual.kind));
                }
            } else if (expected.kind != TYPE_VOID) {
                sema_error(sema, stmt->loc,
                          "Function must return %s", type_name(expected.kind));
            }
            break;
        }

        case STMT_BREAK:
        case STMT_CONTINUE:
            if (!sema->in_loop) {
                sema_error(sema, stmt->loc,
                          "%s outside of loop",
                          stmt->kind == STMT_BREAK ? "break" : "continue");
            }
            break;

        case STMT_BLOCK:
            analyze_block(sema, stmt);
            break;

        default:
            break;
    }
}

// ============================================================================
// Declaration Analysis
// ============================================================================

static void analyze_function(SemanticAnalyzer* sema, AstDecl* decl) {
    FunctionDecl* fn = &decl->as.function;

    // Create function scope
    Scope* scope = scope_new(sema->current_scope);
    sema->current_scope = scope;
    sema->current_function = decl;

    // Add parameters to scope
    for (uint32_t i = 0; i < fn->param_count; i++) {
        Symbol* param = malloc(sizeof(Symbol));
        param->name = strdup(fn->params[i].name);
        param->kind = SYM_PARAMETER;
        param->type = type_from_annotation(&fn->params[i].type);
        param->defined_at = fn->loc;
        param->next = NULL;
        scope_add(scope, param);
    }

    // Analyze body
    if (fn->body) {
        for (uint32_t i = 0; i < fn->body->as.block.stmt_count; i++) {
            analyze_stmt(sema, fn->body->as.block.stmts[i]);
        }
    }

    sema->current_function = NULL;
    sema->current_scope = scope->parent;
    scope_free(scope);
}

static void analyze_agent(SemanticAnalyzer* sema, AstDecl* decl) {
    AgentDecl* agent = &decl->as.agent;

    // Validate required fields
    if (!agent->model) {
        sema_error(sema, decl->loc, "Agent '%s' must specify a model", agent->name);
    }

    // Analyze tools - treat each tool body like a function
    for (uint32_t i = 0; i < agent->tool_count; i++) {
        ToolDecl* tool = &agent->tools[i];

        // Create a synthetic function declaration for the tool
        // so that return statements work properly
        AstDecl tool_as_func;
        tool_as_func.kind = DECL_FUNCTION;
        tool_as_func.loc = tool->loc;
        tool_as_func.as.function.name = tool->name;
        tool_as_func.as.function.params = tool->params;
        tool_as_func.as.function.param_count = tool->param_count;
        tool_as_func.as.function.return_type = tool->return_type;
        tool_as_func.as.function.body = tool->body;

        // Set up function context for return statements
        AstDecl* saved_function = sema->current_function;
        sema->current_function = &tool_as_func;

        // Create scope for tool body
        Scope* scope = scope_new(sema->current_scope);
        sema->current_scope = scope;

        // Add parameters
        for (uint32_t j = 0; j < tool->param_count; j++) {
            Symbol* param = malloc(sizeof(Symbol));
            param->name = strdup(tool->params[j].name);
            param->kind = SYM_PARAMETER;
            param->type = type_from_annotation(&tool->params[j].type);
            param->defined_at = tool->loc;
            param->next = NULL;
            scope_add(scope, param);
        }

        // Analyze body
        if (tool->body) {
            for (uint32_t j = 0; j < tool->body->as.block.stmt_count; j++) {
                analyze_stmt(sema, tool->body->as.block.stmts[j]);
            }
        }

        sema->current_scope = scope->parent;
        scope_free(scope);

        // Restore function context
        sema->current_function = saved_function;
    }
}

static void register_declarations(SemanticAnalyzer* sema, AstProgram* program) {
    // First pass: register all agents and functions
    for (uint32_t i = 0; i < program->decl_count; i++) {
        AstDecl* decl = program->decls[i];

        if (decl->kind == DECL_AGENT) {
            Symbol* sym = malloc(sizeof(Symbol));
            sym->name = strdup(decl->as.agent.name);
            sym->kind = SYM_AGENT;
            sym->type = (TypeInfo){.kind = TYPE_AGENT,
                                   .agent_name = strdup(decl->as.agent.name)};
            sym->defined_at = decl->loc;
            sym->tool_count = decl->as.agent.tool_count;
            sym->tool_names = malloc(sym->tool_count * sizeof(char*));
            for (uint32_t j = 0; j < sym->tool_count; j++) {
                sym->tool_names[j] = strdup(decl->as.agent.tools[j].name);
            }
            sym->next = NULL;
            scope_add(sema->global_scope, sym);
        }
        else if (decl->kind == DECL_FUNCTION) {
            Symbol* sym = malloc(sizeof(Symbol));
            sym->name = strdup(decl->as.function.name);
            sym->kind = SYM_FUNCTION;
            sym->type = (TypeInfo){.kind = TYPE_VOID};
            sym->return_type = type_from_annotation(&decl->as.function.return_type);
            sym->param_count = decl->as.function.param_count;
            sym->param_types = malloc(sym->param_count * sizeof(TypeInfo));
            for (uint32_t j = 0; j < sym->param_count; j++) {
                sym->param_types[j] = type_from_annotation(&decl->as.function.params[j].type);
            }
            sym->defined_at = decl->loc;
            sym->next = NULL;
            scope_add(sema->global_scope, sym);
        }
    }
}

// ============================================================================
// Public API
// ============================================================================

void sema_init(SemanticAnalyzer* sema) {
    sema->global_scope = scope_new(NULL);
    sema->current_scope = sema->global_scope;
    sema->had_error = false;
    sema->error_msg[0] = '\0';
    sema->current_function = NULL;
    sema->current_agent = NULL;
    sema->in_loop = false;
    sema->current_file = NULL;
    module_cache_init(&sema->modules);
}

void sema_cleanup(SemanticAnalyzer* sema) {
    scope_free(sema->global_scope);
    sema->global_scope = NULL;
    sema->current_scope = NULL;
    module_cache_free(&sema->modules);
}

void sema_add_search_path(SemanticAnalyzer* sema, const char* path) {
    if (sema->modules.search_path_count < 8) {
        sema->modules.search_paths[sema->modules.search_path_count++] = strdup(path);
    }
}

bool sema_analyze(SemanticAnalyzer* sema, AstProgram* program, const char* source_path) {
    // Set current file for import resolution
    sema->current_file = source_path;

    // Process imports first
    for (uint32_t i = 0; i < program->decl_count; i++) {
        if (program->decls[i]->kind == DECL_IMPORT) {
            if (!process_import(sema, &program->decls[i]->as.import, program->decls[i]->loc)) {
                return false;
            }
        }
    }

    // Register all declarations (allows forward references)
    register_declarations(sema, program);

    // Check for main function
    Symbol* main_sym = scope_lookup(sema->global_scope, "main");
    if (!main_sym || main_sym->kind != SYM_FUNCTION) {
        fprintf(stderr, "warning: no main function defined\n");
    }

    // Analyze each declaration
    for (uint32_t i = 0; i < program->decl_count; i++) {
        AstDecl* decl = program->decls[i];

        if (decl->kind == DECL_AGENT) {
            analyze_agent(sema, decl);
        }
        else if (decl->kind == DECL_FUNCTION) {
            analyze_function(sema, decl);
        }
    }

    return !sema->had_error;
}

bool sema_had_error(SemanticAnalyzer* sema) {
    return sema->had_error;
}

const char* sema_error_msg(SemanticAnalyzer* sema) {
    return sema->error_msg;
}

SourceLoc sema_error_loc(SemanticAnalyzer* sema) {
    return sema->error_loc;
}

uint32_t sema_get_module_programs(SemanticAnalyzer* sema, AstProgram** programs, uint32_t max_count) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < sema->modules.capacity && count < max_count; i++) {
        Module* mod = sema->modules.modules[i];
        while (mod && count < max_count) {
            if (mod->ast) {
                programs[count++] = mod->ast;
            }
            mod = mod->next;
        }
    }
    return count;
}
