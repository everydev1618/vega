#include "ast.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// Helper Functions
// ============================================================================

static char* strdup_safe(const char* str) {
    if (!str) return NULL;
    size_t len = strlen(str) + 1;
    char* dup = malloc(len);
    if (dup) memcpy(dup, str, len);
    return dup;
}

static char* strndup_safe(const char* str, size_t n) {
    if (!str) return NULL;
    char* dup = malloc(n + 1);
    if (dup) {
        memcpy(dup, str, n);
        dup[n] = '\0';
    }
    return dup;
}

// ============================================================================
// Expression Constructors
// ============================================================================

AstExpr* ast_int_literal(int64_t value, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_INT_LITERAL;
    expr->loc = loc;
    expr->as.int_val = value;
    return expr;
}

AstExpr* ast_float_literal(double value, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_FLOAT_LITERAL;
    expr->loc = loc;
    expr->as.float_val = value;
    return expr;
}

AstExpr* ast_string_literal(const char* value, uint32_t length, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_STRING_LITERAL;
    expr->loc = loc;
    expr->as.string_val.value = strndup_safe(value, length);
    expr->as.string_val.length = length;
    return expr;
}

AstExpr* ast_bool_literal(bool value, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_BOOL_LITERAL;
    expr->loc = loc;
    expr->as.bool_val = value;
    return expr;
}

AstExpr* ast_null_literal(SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_NULL_LITERAL;
    expr->loc = loc;
    return expr;
}

AstExpr* ast_identifier(const char* name, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_IDENTIFIER;
    expr->loc = loc;
    expr->as.ident.name = strdup_safe(name);
    return expr;
}

AstExpr* ast_binary(BinaryOp op, AstExpr* left, AstExpr* right, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_BINARY;
    expr->loc = loc;
    expr->as.binary.op = op;
    expr->as.binary.left = left;
    expr->as.binary.right = right;
    return expr;
}

AstExpr* ast_unary(UnaryOp op, AstExpr* operand, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_UNARY;
    expr->loc = loc;
    expr->as.unary.op = op;
    expr->as.unary.operand = operand;
    return expr;
}

AstExpr* ast_call(AstExpr* callee, AstExpr** args, uint32_t arg_count, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_CALL;
    expr->loc = loc;
    expr->as.call.callee = callee;
    expr->as.call.args = args;
    expr->as.call.arg_count = arg_count;
    return expr;
}

AstExpr* ast_method_call(AstExpr* object, const char* method, AstExpr** args, uint32_t arg_count, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_METHOD_CALL;
    expr->loc = loc;
    expr->as.method_call.object = object;
    expr->as.method_call.method = strdup_safe(method);
    expr->as.method_call.args = args;
    expr->as.method_call.arg_count = arg_count;
    return expr;
}

AstExpr* ast_field_access(AstExpr* object, const char* field, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_FIELD_ACCESS;
    expr->loc = loc;
    expr->as.field_access.object = object;
    expr->as.field_access.field = strdup_safe(field);
    return expr;
}

AstExpr* ast_spawn(const char* agent_name, bool is_async, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_SPAWN;
    expr->loc = loc;
    expr->as.spawn.agent_name = strdup_safe(agent_name);
    expr->as.spawn.is_async = is_async;
    expr->as.spawn.is_supervised = false;
    expr->as.spawn.supervision = NULL;
    return expr;
}

AstExpr* ast_spawn_supervised(const char* agent_name, AstSupervisionConfig* config, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_SPAWN;
    expr->loc = loc;
    expr->as.spawn.agent_name = strdup_safe(agent_name);
    expr->as.spawn.is_async = false;
    expr->as.spawn.is_supervised = true;
    expr->as.spawn.supervision = config;
    return expr;
}

AstSupervisionConfig* ast_supervision_config(AstRestartStrategy strategy, uint32_t max_restarts, uint32_t window_ms) {
    AstSupervisionConfig* config = malloc(sizeof(AstSupervisionConfig));
    if (!config) return NULL;
    config->strategy = strategy;
    config->max_restarts = max_restarts;
    config->window_ms = window_ms;
    return config;
}

AstExpr* ast_message(AstExpr* target, AstExpr* message, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_MESSAGE;
    expr->loc = loc;
    expr->as.message.target = target;
    expr->as.message.message = message;
    return expr;
}

AstExpr* ast_await(AstExpr* future, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_AWAIT;
    expr->loc = loc;
    expr->as.await.future = future;
    return expr;
}

AstExpr* ast_array_literal(AstExpr** elements, uint32_t count, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_ARRAY_LITERAL;
    expr->loc = loc;
    expr->as.array_literal.elements = elements;
    expr->as.array_literal.count = count;
    return expr;
}

AstExpr* ast_index(AstExpr* object, AstExpr* index, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_INDEX;
    expr->loc = loc;
    expr->as.index.object = object;
    expr->as.index.index = index;
    return expr;
}

AstExpr* ast_ok(AstExpr* value, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_OK;
    expr->loc = loc;
    expr->as.result_val.value = value;
    return expr;
}

AstExpr* ast_err(AstExpr* value, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_ERR;
    expr->loc = loc;
    expr->as.result_val.value = value;
    return expr;
}

AstExpr* ast_match(AstExpr* scrutinee, MatchArm* arms, uint32_t arm_count, SourceLoc loc) {
    AstExpr* expr = malloc(sizeof(AstExpr));
    if (!expr) return NULL;
    expr->kind = EXPR_MATCH;
    expr->loc = loc;
    expr->as.match.scrutinee = scrutinee;
    expr->as.match.arms = arms;
    expr->as.match.arm_count = arm_count;
    return expr;
}

// ============================================================================
// Statement Constructors
// ============================================================================

AstStmt* ast_expr_stmt(AstExpr* expr, SourceLoc loc) {
    AstStmt* stmt = malloc(sizeof(AstStmt));
    if (!stmt) return NULL;
    stmt->kind = STMT_EXPR;
    stmt->loc = loc;
    stmt->as.expr.expr = expr;
    return stmt;
}

AstStmt* ast_let_stmt(const char* name, TypeAnnotation* type, AstExpr* init, SourceLoc loc) {
    AstStmt* stmt = malloc(sizeof(AstStmt));
    if (!stmt) return NULL;
    stmt->kind = STMT_LET;
    stmt->loc = loc;
    stmt->as.let.name = strdup_safe(name);
    stmt->as.let.type = type;
    stmt->as.let.init = init;
    return stmt;
}

AstStmt* ast_assign_stmt(AstExpr* target, AstExpr* value, SourceLoc loc) {
    AstStmt* stmt = malloc(sizeof(AstStmt));
    if (!stmt) return NULL;
    stmt->kind = STMT_ASSIGN;
    stmt->loc = loc;
    stmt->as.assign.target = target;
    stmt->as.assign.value = value;
    return stmt;
}

AstStmt* ast_if_stmt(AstExpr* cond, AstStmt* then_b, AstStmt* else_b, SourceLoc loc) {
    AstStmt* stmt = malloc(sizeof(AstStmt));
    if (!stmt) return NULL;
    stmt->kind = STMT_IF;
    stmt->loc = loc;
    stmt->as.if_stmt.condition = cond;
    stmt->as.if_stmt.then_branch = then_b;
    stmt->as.if_stmt.else_branch = else_b;
    return stmt;
}

AstStmt* ast_while_stmt(AstExpr* cond, AstStmt* body, SourceLoc loc) {
    AstStmt* stmt = malloc(sizeof(AstStmt));
    if (!stmt) return NULL;
    stmt->kind = STMT_WHILE;
    stmt->loc = loc;
    stmt->as.while_stmt.condition = cond;
    stmt->as.while_stmt.body = body;
    return stmt;
}

AstStmt* ast_return_stmt(AstExpr* value, SourceLoc loc) {
    AstStmt* stmt = malloc(sizeof(AstStmt));
    if (!stmt) return NULL;
    stmt->kind = STMT_RETURN;
    stmt->loc = loc;
    stmt->as.return_stmt.value = value;
    return stmt;
}

AstStmt* ast_break_stmt(SourceLoc loc) {
    AstStmt* stmt = malloc(sizeof(AstStmt));
    if (!stmt) return NULL;
    stmt->kind = STMT_BREAK;
    stmt->loc = loc;
    return stmt;
}

AstStmt* ast_continue_stmt(SourceLoc loc) {
    AstStmt* stmt = malloc(sizeof(AstStmt));
    if (!stmt) return NULL;
    stmt->kind = STMT_CONTINUE;
    stmt->loc = loc;
    return stmt;
}

AstStmt* ast_block_stmt(AstStmt** stmts, uint32_t count, SourceLoc loc) {
    AstStmt* stmt = malloc(sizeof(AstStmt));
    if (!stmt) return NULL;
    stmt->kind = STMT_BLOCK;
    stmt->loc = loc;
    stmt->as.block.stmts = stmts;
    stmt->as.block.stmt_count = count;
    return stmt;
}

// ============================================================================
// Type Annotation
// ============================================================================

TypeAnnotation* ast_type_annotation(const char* name, bool is_array) {
    TypeAnnotation* type = malloc(sizeof(TypeAnnotation));
    if (!type) return NULL;
    type->name = strdup_safe(name);
    type->is_array = is_array;
    return type;
}

// ============================================================================
// Declarations
// ============================================================================

AstDecl* ast_import(const char* path, const char* alias, SourceLoc loc) {
    AstDecl* decl = malloc(sizeof(AstDecl));
    if (!decl) return NULL;
    decl->kind = DECL_IMPORT;
    decl->loc = loc;
    decl->as.import.path = strdup_safe(path);
    decl->as.import.alias = alias ? strdup_safe(alias) : NULL;
    return decl;
}

// ============================================================================
// Program
// ============================================================================

AstProgram* ast_program_new(void) {
    AstProgram* program = malloc(sizeof(AstProgram));
    if (!program) return NULL;
    program->decls = NULL;
    program->decl_count = 0;
    return program;
}

void ast_program_add_decl(AstProgram* program, AstDecl* decl) {
    if (!program || !decl) return;

    program->decl_count++;
    program->decls = realloc(program->decls, program->decl_count * sizeof(AstDecl*));
    if (program->decls) {
        program->decls[program->decl_count - 1] = decl;
    }
}

// ============================================================================
// Destruction Functions
// ============================================================================

void ast_type_free(TypeAnnotation* type) {
    if (!type) return;
    free(type->name);
    free(type);
}

void ast_expr_free(AstExpr* expr) {
    if (!expr) return;

    switch (expr->kind) {
        case EXPR_STRING_LITERAL:
            free(expr->as.string_val.value);
            break;
        case EXPR_ARRAY_LITERAL:
            for (uint32_t i = 0; i < expr->as.array_literal.count; i++) {
                ast_expr_free(expr->as.array_literal.elements[i]);
            }
            free(expr->as.array_literal.elements);
            break;
        case EXPR_IDENTIFIER:
            free(expr->as.ident.name);
            break;
        case EXPR_BINARY:
            ast_expr_free(expr->as.binary.left);
            ast_expr_free(expr->as.binary.right);
            break;
        case EXPR_UNARY:
            ast_expr_free(expr->as.unary.operand);
            break;
        case EXPR_CALL:
            ast_expr_free(expr->as.call.callee);
            for (uint32_t i = 0; i < expr->as.call.arg_count; i++) {
                ast_expr_free(expr->as.call.args[i]);
            }
            free(expr->as.call.args);
            break;
        case EXPR_METHOD_CALL:
            ast_expr_free(expr->as.method_call.object);
            free(expr->as.method_call.method);
            for (uint32_t i = 0; i < expr->as.method_call.arg_count; i++) {
                ast_expr_free(expr->as.method_call.args[i]);
            }
            free(expr->as.method_call.args);
            break;
        case EXPR_FIELD_ACCESS:
            ast_expr_free(expr->as.field_access.object);
            free(expr->as.field_access.field);
            break;
        case EXPR_INDEX:
            ast_expr_free(expr->as.index.object);
            ast_expr_free(expr->as.index.index);
            break;
        case EXPR_SPAWN:
            free(expr->as.spawn.agent_name);
            free(expr->as.spawn.supervision);
            break;
        case EXPR_MESSAGE:
            ast_expr_free(expr->as.message.target);
            ast_expr_free(expr->as.message.message);
            break;
        case EXPR_AWAIT:
            ast_expr_free(expr->as.await.future);
            break;
        default:
            break;
    }

    free(expr);
}

void ast_stmt_free(AstStmt* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case STMT_EXPR:
            ast_expr_free(stmt->as.expr.expr);
            break;
        case STMT_LET:
            free(stmt->as.let.name);
            ast_type_free(stmt->as.let.type);
            ast_expr_free(stmt->as.let.init);
            break;
        case STMT_ASSIGN:
            ast_expr_free(stmt->as.assign.target);
            ast_expr_free(stmt->as.assign.value);
            break;
        case STMT_IF:
            ast_expr_free(stmt->as.if_stmt.condition);
            ast_stmt_free(stmt->as.if_stmt.then_branch);
            ast_stmt_free(stmt->as.if_stmt.else_branch);
            break;
        case STMT_WHILE:
            ast_expr_free(stmt->as.while_stmt.condition);
            ast_stmt_free(stmt->as.while_stmt.body);
            break;
        case STMT_FOR:
            ast_stmt_free(stmt->as.for_stmt.init);
            ast_expr_free(stmt->as.for_stmt.condition);
            ast_expr_free(stmt->as.for_stmt.update);
            ast_stmt_free(stmt->as.for_stmt.body);
            break;
        case STMT_RETURN:
            ast_expr_free(stmt->as.return_stmt.value);
            break;
        case STMT_BLOCK:
            for (uint32_t i = 0; i < stmt->as.block.stmt_count; i++) {
                ast_stmt_free(stmt->as.block.stmts[i]);
            }
            free(stmt->as.block.stmts);
            break;
        default:
            break;
    }

    free(stmt);
}

static void tool_decl_free(ToolDecl* tool) {
    if (!tool) return;
    free(tool->name);
    free(tool->description);
    for (uint32_t i = 0; i < tool->param_count; i++) {
        free(tool->params[i].name);
        free(tool->params[i].type.name);
    }
    free(tool->params);
    free(tool->return_type.name);
    ast_stmt_free(tool->body);
}

void ast_decl_free(AstDecl* decl) {
    if (!decl) return;

    switch (decl->kind) {
        case DECL_AGENT:
            free(decl->as.agent.name);
            free(decl->as.agent.model);
            free(decl->as.agent.system_prompt);
            for (uint32_t i = 0; i < decl->as.agent.tool_count; i++) {
                tool_decl_free(&decl->as.agent.tools[i]);
            }
            free(decl->as.agent.tools);
            break;
        case DECL_FUNCTION:
            free(decl->as.function.name);
            for (uint32_t i = 0; i < decl->as.function.param_count; i++) {
                free(decl->as.function.params[i].name);
                free(decl->as.function.params[i].type.name);
            }
            free(decl->as.function.params);
            free(decl->as.function.return_type.name);
            ast_stmt_free(decl->as.function.body);
            break;
        case DECL_TOOL:
            tool_decl_free(&decl->as.tool);
            break;
        case DECL_IMPORT:
            free(decl->as.import.path);
            free(decl->as.import.alias);
            break;
    }

    free(decl);
}

void ast_program_free(AstProgram* program) {
    if (!program) return;

    for (uint32_t i = 0; i < program->decl_count; i++) {
        ast_decl_free(program->decls[i]);
    }
    free(program->decls);
    free(program);
}

// ============================================================================
// Printing Functions
// ============================================================================

static void print_indent(int indent) {
    for (int i = 0; i < indent; i++) {
        printf("  ");
    }
}

static const char* binop_name(BinaryOp op) {
    switch (op) {
        case BINOP_ADD: return "+";
        case BINOP_SUB: return "-";
        case BINOP_MUL: return "*";
        case BINOP_DIV: return "/";
        case BINOP_MOD: return "%";
        case BINOP_EQ:  return "==";
        case BINOP_NE:  return "!=";
        case BINOP_LT:  return "<";
        case BINOP_LE:  return "<=";
        case BINOP_GT:  return ">";
        case BINOP_GE:  return ">=";
        case BINOP_AND: return "&&";
        case BINOP_OR:  return "||";
        default:        return "?";
    }
}

static const char* unop_name(UnaryOp op) {
    switch (op) {
        case UNOP_NEG: return "-";
        case UNOP_NOT: return "!";
        default:       return "?";
    }
}

void ast_print_expr(AstExpr* expr, int indent) {
    if (!expr) {
        print_indent(indent);
        printf("(null)\n");
        return;
    }

    print_indent(indent);

    switch (expr->kind) {
        case EXPR_INT_LITERAL:
            printf("Int(%lld)\n", (long long)expr->as.int_val);
            break;
        case EXPR_FLOAT_LITERAL:
            printf("Float(%f)\n", expr->as.float_val);
            break;
        case EXPR_STRING_LITERAL:
            printf("String(\"%s\")\n", expr->as.string_val.value);
            break;
        case EXPR_BOOL_LITERAL:
            printf("Bool(%s)\n", expr->as.bool_val ? "true" : "false");
            break;
        case EXPR_NULL_LITERAL:
            printf("Null\n");
            break;
        case EXPR_IDENTIFIER:
            printf("Ident(%s)\n", expr->as.ident.name);
            break;
        case EXPR_BINARY:
            printf("Binary(%s)\n", binop_name(expr->as.binary.op));
            ast_print_expr(expr->as.binary.left, indent + 1);
            ast_print_expr(expr->as.binary.right, indent + 1);
            break;
        case EXPR_UNARY:
            printf("Unary(%s)\n", unop_name(expr->as.unary.op));
            ast_print_expr(expr->as.unary.operand, indent + 1);
            break;
        case EXPR_CALL:
            printf("Call\n");
            print_indent(indent + 1);
            printf("callee:\n");
            ast_print_expr(expr->as.call.callee, indent + 2);
            print_indent(indent + 1);
            printf("args:\n");
            for (uint32_t i = 0; i < expr->as.call.arg_count; i++) {
                ast_print_expr(expr->as.call.args[i], indent + 2);
            }
            break;
        case EXPR_METHOD_CALL:
            printf("MethodCall(.%s)\n", expr->as.method_call.method);
            ast_print_expr(expr->as.method_call.object, indent + 1);
            for (uint32_t i = 0; i < expr->as.method_call.arg_count; i++) {
                ast_print_expr(expr->as.method_call.args[i], indent + 1);
            }
            break;
        case EXPR_FIELD_ACCESS:
            printf("FieldAccess(.%s)\n", expr->as.field_access.field);
            ast_print_expr(expr->as.field_access.object, indent + 1);
            break;
        case EXPR_SPAWN:
            printf("Spawn(%s%s%s)\n",
                   expr->as.spawn.is_async ? "async " : "",
                   expr->as.spawn.is_supervised ? "supervised " : "",
                   expr->as.spawn.agent_name);
            if (expr->as.spawn.supervision) {
                print_indent(indent + 1);
                printf("supervision: strategy=%d max=%u window=%ums\n",
                       expr->as.spawn.supervision->strategy,
                       expr->as.spawn.supervision->max_restarts,
                       expr->as.spawn.supervision->window_ms);
            }
            break;
        case EXPR_MESSAGE:
            printf("Message(<-)\n");
            ast_print_expr(expr->as.message.target, indent + 1);
            ast_print_expr(expr->as.message.message, indent + 1);
            break;
        case EXPR_AWAIT:
            printf("Await\n");
            ast_print_expr(expr->as.await.future, indent + 1);
            break;
        default:
            printf("Unknown expr kind\n");
    }
}

void ast_print_stmt(AstStmt* stmt, int indent) {
    if (!stmt) {
        print_indent(indent);
        printf("(null)\n");
        return;
    }

    print_indent(indent);

    switch (stmt->kind) {
        case STMT_EXPR:
            printf("ExprStmt\n");
            ast_print_expr(stmt->as.expr.expr, indent + 1);
            break;
        case STMT_LET:
            printf("Let(%s", stmt->as.let.name);
            if (stmt->as.let.type) {
                printf(": %s%s",
                       stmt->as.let.type->name,
                       stmt->as.let.type->is_array ? "[]" : "");
            }
            printf(")\n");
            if (stmt->as.let.init) {
                ast_print_expr(stmt->as.let.init, indent + 1);
            }
            break;
        case STMT_ASSIGN:
            printf("Assign\n");
            ast_print_expr(stmt->as.assign.target, indent + 1);
            ast_print_expr(stmt->as.assign.value, indent + 1);
            break;
        case STMT_IF:
            printf("If\n");
            print_indent(indent + 1);
            printf("condition:\n");
            ast_print_expr(stmt->as.if_stmt.condition, indent + 2);
            print_indent(indent + 1);
            printf("then:\n");
            ast_print_stmt(stmt->as.if_stmt.then_branch, indent + 2);
            if (stmt->as.if_stmt.else_branch) {
                print_indent(indent + 1);
                printf("else:\n");
                ast_print_stmt(stmt->as.if_stmt.else_branch, indent + 2);
            }
            break;
        case STMT_WHILE:
            printf("While\n");
            ast_print_expr(stmt->as.while_stmt.condition, indent + 1);
            ast_print_stmt(stmt->as.while_stmt.body, indent + 1);
            break;
        case STMT_RETURN:
            printf("Return\n");
            if (stmt->as.return_stmt.value) {
                ast_print_expr(stmt->as.return_stmt.value, indent + 1);
            }
            break;
        case STMT_BREAK:
            printf("Break\n");
            break;
        case STMT_CONTINUE:
            printf("Continue\n");
            break;
        case STMT_BLOCK:
            printf("Block\n");
            for (uint32_t i = 0; i < stmt->as.block.stmt_count; i++) {
                ast_print_stmt(stmt->as.block.stmts[i], indent + 1);
            }
            break;
        default:
            printf("Unknown stmt kind\n");
    }
}

void ast_print_decl(AstDecl* decl, int indent) {
    if (!decl) return;

    print_indent(indent);

    switch (decl->kind) {
        case DECL_AGENT:
            printf("Agent(%s)\n", decl->as.agent.name);
            print_indent(indent + 1);
            printf("model: %s\n", decl->as.agent.model ? decl->as.agent.model : "(none)");
            print_indent(indent + 1);
            printf("system: %s\n", decl->as.agent.system_prompt ? decl->as.agent.system_prompt : "(none)");
            print_indent(indent + 1);
            printf("temperature: %f\n", decl->as.agent.temperature);
            print_indent(indent + 1);
            printf("tools: %u\n", decl->as.agent.tool_count);
            break;
        case DECL_FUNCTION:
            printf("Function(%s)\n", decl->as.function.name);
            print_indent(indent + 1);
            printf("params: %u\n", decl->as.function.param_count);
            print_indent(indent + 1);
            printf("returns: %s\n",
                   decl->as.function.return_type.name ? decl->as.function.return_type.name : "void");
            if (decl->as.function.body) {
                ast_print_stmt(decl->as.function.body, indent + 1);
            }
            break;
        case DECL_TOOL:
            printf("Tool(%s)\n", decl->as.tool.name);
            break;
        case DECL_IMPORT:
            printf("Import(\"%s\"", decl->as.import.path);
            if (decl->as.import.alias) {
                printf(" as %s", decl->as.import.alias);
            }
            printf(")\n");
            break;
    }
}

void ast_print_program(AstProgram* program) {
    if (!program) return;

    printf("Program (%u declarations)\n", program->decl_count);
    for (uint32_t i = 0; i < program->decl_count; i++) {
        ast_print_decl(program->decls[i], 1);
    }
}
