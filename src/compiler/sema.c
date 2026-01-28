#include "sema.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>

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
                        if (strstr(name, "contains")) return (TypeInfo){.kind = TYPE_BOOL};
                        return (TypeInfo){.kind = TYPE_STRING};
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

            if (expr->as.spawn.is_async) {
                return (TypeInfo){.kind = TYPE_FUTURE,
                                  .agent_name = strdup(expr->as.spawn.agent_name)};
            }
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
            if (future.kind != TYPE_FUTURE) {
                sema_error(sema, expr->loc, "Can only await futures");
            }
            return (TypeInfo){.kind = TYPE_STRING};
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
}

void sema_cleanup(SemanticAnalyzer* sema) {
    scope_free(sema->global_scope);
    sema->global_scope = NULL;
    sema->current_scope = NULL;
}

bool sema_analyze(SemanticAnalyzer* sema, AstProgram* program) {
    // Register all declarations first (allows forward references)
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
