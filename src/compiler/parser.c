#include "parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

// ============================================================================
// Helper Functions
// ============================================================================

static void advance(Parser* parser) {
    parser->previous = parser->current;
    parser->current = lexer_next_token(parser->lexer);

    if (parser->current.type == TOK_ERROR) {
        parser->had_error = true;
        parser->error_loc = parser->current.loc;
        snprintf(parser->error_msg, sizeof(parser->error_msg),
                 "%.*s", (int)parser->current.value.str.length,
                 parser->current.value.str.start);
    }
}

static bool check(Parser* parser, TokenType type) {
    return parser->current.type == type;
}

static bool match(Parser* parser, TokenType type) {
    if (!check(parser, type)) return false;
    advance(parser);
    return true;
}

static void error_at(Parser* parser, Token* token, const char* fmt, ...) {
    if (parser->panic_mode) return;
    parser->panic_mode = true;
    parser->had_error = true;
    parser->error_loc = token->loc;

    va_list args;
    va_start(args, fmt);
    vsnprintf(parser->error_msg, sizeof(parser->error_msg), fmt, args);
    va_end(args);

    fprintf(stderr, "%s:%u:%u: error: %s\n",
            token->loc.filename, token->loc.line, token->loc.column,
            parser->error_msg);
}

static void error(Parser* parser, const char* fmt, ...) {
    if (parser->panic_mode) return;
    parser->panic_mode = true;
    parser->had_error = true;
    parser->error_loc = parser->current.loc;

    va_list args;
    va_start(args, fmt);
    vsnprintf(parser->error_msg, sizeof(parser->error_msg), fmt, args);
    va_end(args);

    fprintf(stderr, "%s:%u:%u: error: %s\n",
            parser->current.loc.filename, parser->current.loc.line,
            parser->current.loc.column, parser->error_msg);
}

static bool consume(Parser* parser, TokenType type, const char* msg) {
    if (check(parser, type)) {
        advance(parser);
        return true;
    }
    error(parser, "%s, got %s", msg, token_type_name(parser->current.type));
    return false;
}

static void synchronize(Parser* parser) {
    parser->panic_mode = false;

    while (parser->current.type != TOK_EOF) {
        if (parser->previous.type == TOK_SEMICOLON) return;
        if (parser->previous.type == TOK_RBRACE) return;

        switch (parser->current.type) {
            case TOK_AGENT:
            case TOK_FN:
            case TOK_LET:
            case TOK_IF:
            case TOK_WHILE:
            case TOK_RETURN:
                return;
            default:
                break;
        }
        advance(parser);
    }
}

static char* copy_token_string(Token* token) {
    char* str = malloc(token->value.str.length + 1);
    if (str) {
        memcpy(str, token->value.str.start, token->value.str.length);
        str[token->value.str.length] = '\0';
    }
    return str;
}

// ============================================================================
// Expression Parsing (Pratt Parser)
// ============================================================================

typedef enum {
    PREC_NONE,
    PREC_ASSIGNMENT,    // =
    PREC_OR,            // ||
    PREC_AND,           // &&
    PREC_EQUALITY,      // == !=
    PREC_COMPARISON,    // < > <= >=
    PREC_TERM,          // + -
    PREC_FACTOR,        // * / %
    PREC_UNARY,         // ! -
    PREC_CALL,          // . () []
    PREC_MESSAGE,       // <-
    PREC_PRIMARY
} Precedence;

// Forward declarations
static AstExpr* parse_expression(Parser* parser);
static AstExpr* parse_precedence(Parser* parser, Precedence precedence);
static AstStmt* parse_statement(Parser* parser);
static AstStmt* parse_block(Parser* parser);

// Primary expressions
static AstExpr* parse_number(Parser* parser) {
    Token token = parser->previous;
    if (token.type == TOK_INT) {
        return ast_int_literal(token.value.int_val, token.loc);
    } else {
        return ast_float_literal(token.value.float_val, token.loc);
    }
}

static AstExpr* parse_string(Parser* parser) {
    Token token = parser->previous;
    return ast_string_literal(token.value.str.start, token.value.str.length, token.loc);
}

static AstExpr* parse_identifier(Parser* parser) {
    Token token = parser->previous;
    char* name = copy_token_string(&token);
    return ast_identifier(name, token.loc);
}

static AstExpr* parse_true(Parser* parser) {
    return ast_bool_literal(true, parser->previous.loc);
}

static AstExpr* parse_false(Parser* parser) {
    return ast_bool_literal(false, parser->previous.loc);
}

static AstExpr* parse_null(Parser* parser) {
    return ast_null_literal(parser->previous.loc);
}

static AstExpr* parse_grouping(Parser* parser) {
    AstExpr* expr = parse_expression(parser);
    consume(parser, TOK_RPAREN, "Expected ')' after expression");
    return expr;
}

static AstExpr* parse_unary(Parser* parser) {
    Token op = parser->previous;
    AstExpr* operand = parse_precedence(parser, PREC_UNARY);

    UnaryOp unop;
    if (op.type == TOK_MINUS) {
        unop = UNOP_NEG;
    } else if (op.type == TOK_NOT) {
        unop = UNOP_NOT;
    } else {
        error_at(parser, &op, "Unknown unary operator");
        return operand;
    }

    return ast_unary(unop, operand, op.loc);
}

static AstSupervisionConfig* parse_supervision_config(Parser* parser) {
    // supervised by { ... }
    if (!consume(parser, TOK_BY, "Expected 'by' after 'supervised'")) {
        return NULL;
    }
    if (!consume(parser, TOK_LBRACE, "Expected '{' after 'supervised by'")) {
        return NULL;
    }

    // Defaults
    AstRestartStrategy strategy = RESTART_STRATEGY_RESTART;
    uint32_t max_restarts = 3;
    uint32_t window_ms = 60000;

    while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
        if (match(parser, TOK_STRATEGY)) {
            if (match(parser, TOK_RESTART)) {
                strategy = RESTART_STRATEGY_RESTART;
            } else if (match(parser, TOK_STOP)) {
                strategy = RESTART_STRATEGY_STOP;
            } else if (match(parser, TOK_ESCALATE)) {
                strategy = RESTART_STRATEGY_ESCALATE;
            } else if (match(parser, TOK_RESTART_ALL)) {
                strategy = RESTART_STRATEGY_RESTART_ALL;
            } else {
                error(parser, "Expected restart strategy (restart, stop, escalate, restart_all)");
            }
        }
        else if (match(parser, TOK_MAX_RESTARTS)) {
            if (match(parser, TOK_INT)) {
                max_restarts = (uint32_t)parser->previous.value.int_val;
            } else {
                error(parser, "Expected integer for max_restarts");
            }
        }
        else if (match(parser, TOK_WINDOW)) {
            if (match(parser, TOK_INT)) {
                window_ms = (uint32_t)parser->previous.value.int_val;
            } else {
                error(parser, "Expected integer for window (milliseconds)");
            }
        }
        else {
            error(parser, "Unexpected token in supervision config: %s",
                  token_type_name(parser->current.type));
            advance(parser);
        }
    }

    consume(parser, TOK_RBRACE, "Expected '}' after supervision config");

    return ast_supervision_config(strategy, max_restarts, window_ms);
}

static AstExpr* parse_spawn(Parser* parser) {
    SourceLoc loc = parser->previous.loc;

    // Check for async before agent name: spawn async Worker
    bool is_async = match(parser, TOK_ASYNC);

    if (!consume(parser, TOK_IDENT, "Expected agent name after 'spawn'")) {
        return NULL;
    }

    char* name = copy_token_string(&parser->previous);

    // Also check for async after agent name: spawn Worker async
    if (!is_async) {
        is_async = match(parser, TOK_ASYNC);
    }

    // Check for supervision
    if (match(parser, TOK_SUPERVISED)) {
        AstSupervisionConfig* config = parse_supervision_config(parser);
        if (!config) {
            free(name);
            return NULL;
        }
        return ast_spawn_supervised(name, config, loc);
    }

    return ast_spawn(name, is_async, loc);
}

static AstExpr* parse_await(Parser* parser) {
    SourceLoc loc = parser->previous.loc;
    AstExpr* future = parse_expression(parser);
    return ast_await(future, loc);
}

static AstExpr* parse_ok(Parser* parser) {
    SourceLoc loc = parser->previous.loc;
    consume(parser, TOK_LPAREN, "Expected '(' after 'Ok'");
    AstExpr* value = parse_expression(parser);
    consume(parser, TOK_RPAREN, "Expected ')' after Ok value");
    return ast_ok(value, loc);
}

static AstExpr* parse_err(Parser* parser) {
    SourceLoc loc = parser->previous.loc;
    consume(parser, TOK_LPAREN, "Expected '(' after 'Err'");
    AstExpr* value = parse_expression(parser);
    consume(parser, TOK_RPAREN, "Expected ')' after Err value");
    return ast_err(value, loc);
}

static AstExpr* parse_match(Parser* parser) {
    SourceLoc loc = parser->previous.loc;

    AstExpr* scrutinee = parse_expression(parser);
    consume(parser, TOK_LBRACE, "Expected '{' after match expression");

    MatchArm* arms = NULL;
    uint32_t arm_count = 0;
    uint32_t arm_capacity = 0;

    while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
        MatchArm arm = {0};

        // Parse Ok(x) or Err(x)
        if (match(parser, TOK_OK)) {
            arm.is_ok = true;
        } else if (match(parser, TOK_ERR)) {
            arm.is_ok = false;
        } else {
            error(parser, "Expected 'Ok' or 'Err' in match arm");
            break;
        }

        consume(parser, TOK_LPAREN, "Expected '(' after Ok/Err");
        consume(parser, TOK_IDENT, "Expected variable name in pattern");
        arm.binding_name = copy_token_string(&parser->previous);
        consume(parser, TOK_RPAREN, "Expected ')' after pattern variable");

        consume(parser, TOK_FATARROW, "Expected '=>' after pattern");

        arm.body = parse_expression(parser);

        // Add arm to array
        if (arm_count >= arm_capacity) {
            arm_capacity = arm_capacity == 0 ? 2 : arm_capacity * 2;
            arms = realloc(arms, arm_capacity * sizeof(MatchArm));
        }
        arms[arm_count++] = arm;

        // Optional comma between arms
        match(parser, TOK_COMMA);
    }

    consume(parser, TOK_RBRACE, "Expected '}' after match arms");
    return ast_match(scrutinee, arms, arm_count, loc);
}

// Array literal: [expr, expr, ...]
static AstExpr* parse_array_literal(Parser* parser) {
    SourceLoc loc = parser->previous.loc;

    AstExpr** elements = NULL;
    uint32_t count = 0;
    uint32_t capacity = 0;

    if (!check(parser, TOK_RBRACKET)) {
        do {
            if (count >= capacity) {
                capacity = capacity == 0 ? 4 : capacity * 2;
                elements = realloc(elements, capacity * sizeof(AstExpr*));
            }
            elements[count++] = parse_expression(parser);
        } while (match(parser, TOK_COMMA));
    }

    consume(parser, TOK_RBRACKET, "Expected ']' after array elements");
    return ast_array_literal(elements, count, loc);
}

// Infix/binary expressions
static BinaryOp token_to_binop(TokenType type) {
    switch (type) {
        case TOK_PLUS:   return BINOP_ADD;
        case TOK_MINUS:  return BINOP_SUB;
        case TOK_STAR:   return BINOP_MUL;
        case TOK_SLASH:  return BINOP_DIV;
        case TOK_PERCENT: return BINOP_MOD;
        case TOK_EQEQ:   return BINOP_EQ;
        case TOK_NE:     return BINOP_NE;
        case TOK_LT:     return BINOP_LT;
        case TOK_LE:     return BINOP_LE;
        case TOK_GT:     return BINOP_GT;
        case TOK_GE:     return BINOP_GE;
        case TOK_AND:    return BINOP_AND;
        case TOK_OR:     return BINOP_OR;
        default:         return BINOP_ADD; // shouldn't happen
    }
}

static AstExpr* parse_binary(Parser* parser, AstExpr* left) {
    Token op = parser->previous;
    Precedence prec;

    switch (op.type) {
        case TOK_OR:      prec = PREC_OR; break;
        case TOK_AND:     prec = PREC_AND; break;
        case TOK_EQEQ:
        case TOK_NE:      prec = PREC_EQUALITY; break;
        case TOK_LT:
        case TOK_LE:
        case TOK_GT:
        case TOK_GE:      prec = PREC_COMPARISON; break;
        case TOK_PLUS:
        case TOK_MINUS:   prec = PREC_TERM; break;
        case TOK_STAR:
        case TOK_SLASH:
        case TOK_PERCENT: prec = PREC_FACTOR; break;
        default:          prec = PREC_NONE; break;
    }

    AstExpr* right = parse_precedence(parser, (Precedence)(prec + 1));
    return ast_binary(token_to_binop(op.type), left, right, op.loc);
}

static AstExpr* parse_message(Parser* parser, AstExpr* left) {
    SourceLoc loc = parser->previous.loc;
    bool is_async = (parser->previous.type == TOK_MSG_ASYNC);
    AstExpr* message = parse_expression(parser);
    return ast_message(left, message, is_async, loc);
}

static AstExpr* parse_call(Parser* parser, AstExpr* callee) {
    SourceLoc loc = parser->previous.loc;

    // Parse arguments
    AstExpr** args = NULL;
    uint32_t arg_count = 0;
    uint32_t arg_capacity = 0;

    if (!check(parser, TOK_RPAREN)) {
        do {
            if (arg_count >= arg_capacity) {
                arg_capacity = arg_capacity == 0 ? 4 : arg_capacity * 2;
                args = realloc(args, arg_capacity * sizeof(AstExpr*));
            }
            args[arg_count++] = parse_expression(parser);
        } while (match(parser, TOK_COMMA));
    }

    consume(parser, TOK_RPAREN, "Expected ')' after arguments");

    return ast_call(callee, args, arg_count, loc);
}

static AstExpr* parse_dot(Parser* parser, AstExpr* left) {
    consume(parser, TOK_IDENT, "Expected property name after '.'");
    char* name = copy_token_string(&parser->previous);
    SourceLoc loc = parser->previous.loc;

    // Check if this is a method call
    if (match(parser, TOK_LPAREN)) {
        AstExpr** args = NULL;
        uint32_t arg_count = 0;
        uint32_t arg_capacity = 0;

        if (!check(parser, TOK_RPAREN)) {
            do {
                if (arg_count >= arg_capacity) {
                    arg_capacity = arg_capacity == 0 ? 4 : arg_capacity * 2;
                    args = realloc(args, arg_capacity * sizeof(AstExpr*));
                }
                args[arg_count++] = parse_expression(parser);
            } while (match(parser, TOK_COMMA));
        }

        consume(parser, TOK_RPAREN, "Expected ')' after method arguments");
        return ast_method_call(left, name, args, arg_count, loc);
    }

    return ast_field_access(left, name, loc);
}

// Index expression: arr[idx]
static AstExpr* parse_index(Parser* parser, AstExpr* left) {
    SourceLoc loc = parser->previous.loc;
    AstExpr* index = parse_expression(parser);
    consume(parser, TOK_RBRACKET, "Expected ']' after index");
    return ast_index(left, index, loc);
}

// Module call: module::function(args)
static AstExpr* parse_module_call(Parser* parser, AstExpr* module_ident) {
    // We've just consumed ::
    SourceLoc loc = parser->previous.loc;

    consume(parser, TOK_IDENT, "Expected function name after '::'");
    char* func_name = copy_token_string(&parser->previous);

    // Build qualified name
    if (module_ident->kind != EXPR_IDENTIFIER) {
        error(parser, "Module path must be an identifier");
        return module_ident;
    }

    char qualified[256];
    snprintf(qualified, sizeof(qualified), "%s::%s",
             module_ident->as.ident.name, func_name);
    free(func_name);

    AstExpr* callee = ast_identifier(strdup(qualified), loc);
    ast_expr_free(module_ident);

    // Must be followed by (
    if (!consume(parser, TOK_LPAREN, "Expected '(' after module function")) {
        return callee;
    }

    AstExpr** args = NULL;
    uint32_t arg_count = 0;
    uint32_t arg_capacity = 0;

    if (!check(parser, TOK_RPAREN)) {
        do {
            if (arg_count >= arg_capacity) {
                arg_capacity = arg_capacity == 0 ? 4 : arg_capacity * 2;
                args = realloc(args, arg_capacity * sizeof(AstExpr*));
            }
            args[arg_count++] = parse_expression(parser);
        } while (match(parser, TOK_COMMA));
    }

    consume(parser, TOK_RPAREN, "Expected ')' after arguments");
    return ast_call(callee, args, arg_count, loc);
}

// Get prefix parsing function
static AstExpr* parse_prefix(Parser* parser) {
    switch (parser->previous.type) {
        case TOK_INT:
        case TOK_FLOAT:    return parse_number(parser);
        case TOK_STRING:   return parse_string(parser);
        case TOK_IDENT:    return parse_identifier(parser);
        case TOK_TRUE:     return parse_true(parser);
        case TOK_FALSE:    return parse_false(parser);
        case TOK_NULL:     return parse_null(parser);
        case TOK_LPAREN:   return parse_grouping(parser);
        case TOK_LBRACKET: return parse_array_literal(parser);
        case TOK_MINUS:
        case TOK_NOT:      return parse_unary(parser);
        case TOK_SPAWN:    return parse_spawn(parser);
        case TOK_AWAIT:    return parse_await(parser);
        case TOK_OK:       return parse_ok(parser);
        case TOK_ERR:      return parse_err(parser);
        case TOK_MATCH:    return parse_match(parser);
        default:
            error(parser, "Expected expression, got %s",
                  token_type_name(parser->previous.type));
            return NULL;
    }
}

static Precedence get_infix_precedence(TokenType type) {
    switch (type) {
        case TOK_MSG:
        case TOK_MSG_ASYNC: return PREC_MESSAGE;
        case TOK_OR:       return PREC_OR;
        case TOK_AND:      return PREC_AND;
        case TOK_EQEQ:
        case TOK_NE:       return PREC_EQUALITY;
        case TOK_LT:
        case TOK_LE:
        case TOK_GT:
        case TOK_GE:       return PREC_COMPARISON;
        case TOK_PLUS:
        case TOK_MINUS:    return PREC_TERM;
        case TOK_STAR:
        case TOK_SLASH:
        case TOK_PERCENT:  return PREC_FACTOR;
        case TOK_LPAREN:
        case TOK_LBRACKET:
        case TOK_DOT:
        case TOK_COLONCOLON: return PREC_CALL;
        default:           return PREC_NONE;
    }
}

static AstExpr* parse_infix(Parser* parser, AstExpr* left) {
    switch (parser->previous.type) {
        case TOK_MSG:
        case TOK_MSG_ASYNC:  return parse_message(parser, left);
        case TOK_PLUS:
        case TOK_MINUS:
        case TOK_STAR:
        case TOK_SLASH:
        case TOK_PERCENT:
        case TOK_EQEQ:
        case TOK_NE:
        case TOK_LT:
        case TOK_LE:
        case TOK_GT:
        case TOK_GE:
        case TOK_AND:
        case TOK_OR:         return parse_binary(parser, left);
        case TOK_LPAREN:     return parse_call(parser, left);
        case TOK_LBRACKET:   return parse_index(parser, left);
        case TOK_DOT:        return parse_dot(parser, left);
        case TOK_COLONCOLON: return parse_module_call(parser, left);
        default:
            return left;
    }
}

static AstExpr* parse_precedence(Parser* parser, Precedence precedence) {
    advance(parser);

    AstExpr* left = parse_prefix(parser);
    if (!left) return NULL;

    while (precedence <= get_infix_precedence(parser->current.type)) {
        advance(parser);
        left = parse_infix(parser, left);
        if (!left) return NULL;
    }

    return left;
}

static AstExpr* parse_expression(Parser* parser) {
    return parse_precedence(parser, PREC_ASSIGNMENT);
}

// ============================================================================
// Statement Parsing
// ============================================================================

static AstStmt* parse_let_statement(Parser* parser) {
    SourceLoc loc = parser->previous.loc;

    consume(parser, TOK_IDENT, "Expected variable name");
    char* name = copy_token_string(&parser->previous);

    // Optional type annotation
    TypeAnnotation* type = NULL;
    if (match(parser, TOK_COLON)) {
        consume(parser, TOK_IDENT, "Expected type name");
        char* type_name = copy_token_string(&parser->previous);
        bool is_array = match(parser, TOK_LBRACKET) && match(parser, TOK_RBRACKET);
        type = ast_type_annotation(type_name, is_array);
    }

    // Optional initializer
    AstExpr* init = NULL;
    if (match(parser, TOK_EQ)) {
        init = parse_expression(parser);
    }

    consume(parser, TOK_SEMICOLON, "Expected ';' after variable declaration");
    return ast_let_stmt(name, type, init, loc);
}

static AstStmt* parse_if_statement(Parser* parser) {
    SourceLoc loc = parser->previous.loc;

    AstExpr* condition = parse_expression(parser);
    AstStmt* then_branch = parse_block(parser);

    AstStmt* else_branch = NULL;
    if (match(parser, TOK_ELSE)) {
        if (check(parser, TOK_IF)) {
            advance(parser);
            else_branch = parse_if_statement(parser);
        } else {
            else_branch = parse_block(parser);
        }
    }

    return ast_if_stmt(condition, then_branch, else_branch, loc);
}

static AstStmt* parse_while_statement(Parser* parser) {
    SourceLoc loc = parser->previous.loc;

    AstExpr* condition = parse_expression(parser);
    AstStmt* body = parse_block(parser);

    return ast_while_stmt(condition, body, loc);
}

static AstStmt* parse_for_statement(Parser* parser) {
    // for init; condition; update { body }
    // or: for let x = 0; x < 10; x = x + 1 { body }
    SourceLoc loc = parser->previous.loc;

    // Init (can be let statement or assignment expression)
    AstStmt* init = NULL;
    if (match(parser, TOK_LET)) {
        // let x = 0 (without semicolon - we'll handle it)
        consume(parser, TOK_IDENT, "Expected variable name");
        char* name = copy_token_string(&parser->previous);

        TypeAnnotation* type = NULL;
        if (match(parser, TOK_COLON)) {
            consume(parser, TOK_IDENT, "Expected type name");
            char* type_name = copy_token_string(&parser->previous);
            bool is_array = match(parser, TOK_LBRACKET) && match(parser, TOK_RBRACKET);
            type = ast_type_annotation(type_name, is_array);
        }

        AstExpr* init_expr = NULL;
        if (match(parser, TOK_EQ)) {
            init_expr = parse_expression(parser);
        }

        init = ast_let_stmt(name, type, init_expr, loc);
    } else if (!check(parser, TOK_SEMICOLON)) {
        // Expression-based init (e.g., i = 0)
        AstExpr* expr = parse_expression(parser);
        if (match(parser, TOK_EQ)) {
            AstExpr* value = parse_expression(parser);
            init = ast_assign_stmt(expr, value, loc);
        } else {
            init = ast_expr_stmt(expr, loc);
        }
    }

    consume(parser, TOK_SEMICOLON, "Expected ';' after for init");

    // Condition
    AstExpr* condition = NULL;
    if (!check(parser, TOK_SEMICOLON)) {
        condition = parse_expression(parser);
    }

    consume(parser, TOK_SEMICOLON, "Expected ';' after for condition");

    // Update
    AstExpr* update = NULL;
    if (!check(parser, TOK_LBRACE)) {
        update = parse_expression(parser);
        // Check for assignment in update
        if (match(parser, TOK_EQ)) {
            // This is actually: i = i + 1, so we need to handle differently
            // For now, we'll just parse the value and create a combined expression
            // Actually the expression parser will handle assignment
            // Let's re-parse as we consumed too much
        }
    }

    // Body
    AstStmt* body = parse_block(parser);

    // Construct for statement
    AstStmt* stmt = malloc(sizeof(AstStmt));
    stmt->kind = STMT_FOR;
    stmt->loc = loc;
    stmt->as.for_stmt.init = init;
    stmt->as.for_stmt.condition = condition;
    stmt->as.for_stmt.update = update;
    stmt->as.for_stmt.body = body;

    return stmt;
}

static AstStmt* parse_return_statement(Parser* parser) {
    SourceLoc loc = parser->previous.loc;

    AstExpr* value = NULL;
    if (!check(parser, TOK_SEMICOLON)) {
        value = parse_expression(parser);
    }

    consume(parser, TOK_SEMICOLON, "Expected ';' after return value");
    return ast_return_stmt(value, loc);
}

static AstStmt* parse_block(Parser* parser) {
    SourceLoc loc = parser->current.loc;
    consume(parser, TOK_LBRACE, "Expected '{'");

    AstStmt** stmts = NULL;
    uint32_t stmt_count = 0;
    uint32_t stmt_capacity = 0;

    while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
        AstStmt* stmt = parse_statement(parser);
        if (stmt) {
            if (stmt_count >= stmt_capacity) {
                stmt_capacity = stmt_capacity == 0 ? 8 : stmt_capacity * 2;
                stmts = realloc(stmts, stmt_capacity * sizeof(AstStmt*));
            }
            stmts[stmt_count++] = stmt;
        }

        if (parser->panic_mode) {
            synchronize(parser);
        }
    }

    consume(parser, TOK_RBRACE, "Expected '}'");
    return ast_block_stmt(stmts, stmt_count, loc);
}

static AstStmt* parse_expression_statement(Parser* parser) {
    SourceLoc loc = parser->current.loc;
    AstExpr* expr = parse_expression(parser);

    // Check for assignment
    if (match(parser, TOK_EQ)) {
        AstExpr* value = parse_expression(parser);
        consume(parser, TOK_SEMICOLON, "Expected ';' after assignment");
        return ast_assign_stmt(expr, value, loc);
    }

    consume(parser, TOK_SEMICOLON, "Expected ';' after expression");
    return ast_expr_stmt(expr, loc);
}

static AstStmt* parse_statement(Parser* parser) {
    if (match(parser, TOK_LET)) {
        return parse_let_statement(parser);
    }
    if (match(parser, TOK_IF)) {
        return parse_if_statement(parser);
    }
    if (match(parser, TOK_WHILE)) {
        return parse_while_statement(parser);
    }
    if (match(parser, TOK_FOR)) {
        return parse_for_statement(parser);
    }
    if (match(parser, TOK_RETURN)) {
        return parse_return_statement(parser);
    }
    if (match(parser, TOK_BREAK)) {
        SourceLoc loc = parser->previous.loc;
        consume(parser, TOK_SEMICOLON, "Expected ';' after 'break'");
        return ast_break_stmt(loc);
    }
    if (match(parser, TOK_CONTINUE)) {
        SourceLoc loc = parser->previous.loc;
        consume(parser, TOK_SEMICOLON, "Expected ';' after 'continue'");
        return ast_continue_stmt(loc);
    }
    if (check(parser, TOK_LBRACE)) {
        return parse_block(parser);
    }

    // Match statements don't need trailing semicolon
    if (match(parser, TOK_MATCH)) {
        AstExpr* match_expr = parse_match(parser);
        return ast_expr_stmt(match_expr, match_expr->loc);
    }

    return parse_expression_statement(parser);
}

// ============================================================================
// Declaration Parsing
// ============================================================================

static TypeAnnotation parse_type(Parser* parser) {
    TypeAnnotation type = {0};

    // Check for Result type
    if (check(parser, TOK_IDENT) &&
        parser->current.value.str.length == 6 &&
        strncmp(parser->current.value.str.start, "Result", 6) == 0) {
        advance(parser);
        type.name = strdup("Result");
        type.is_result = true;

        // Parse Result<T, E>
        if (match(parser, TOK_LT)) {
            type.ok_type = malloc(sizeof(TypeAnnotation));
            *type.ok_type = parse_type(parser);

            consume(parser, TOK_COMMA, "Expected ',' in Result<T, E>");

            type.err_type = malloc(sizeof(TypeAnnotation));
            *type.err_type = parse_type(parser);

            consume(parser, TOK_GT, "Expected '>' after Result<T, E>");
        }
        return type;
    }

    consume(parser, TOK_IDENT, "Expected type name");
    type.name = copy_token_string(&parser->previous);
    type.is_array = match(parser, TOK_LBRACKET) && match(parser, TOK_RBRACKET);
    type.is_result = false;
    type.ok_type = NULL;
    type.err_type = NULL;
    return type;
}

static Parameter* parse_parameters(Parser* parser, uint32_t* count) {
    Parameter* params = NULL;
    uint32_t param_count = 0;
    uint32_t param_capacity = 0;

    consume(parser, TOK_LPAREN, "Expected '('");

    if (!check(parser, TOK_RPAREN)) {
        do {
            consume(parser, TOK_IDENT, "Expected parameter name");
            char* name = copy_token_string(&parser->previous);

            consume(parser, TOK_COLON, "Expected ':' after parameter name");
            TypeAnnotation type = parse_type(parser);

            if (param_count >= param_capacity) {
                param_capacity = param_capacity == 0 ? 4 : param_capacity * 2;
                params = realloc(params, param_capacity * sizeof(Parameter));
            }
            params[param_count].name = name;
            params[param_count].type = type;
            param_count++;
        } while (match(parser, TOK_COMMA));
    }

    consume(parser, TOK_RPAREN, "Expected ')'");
    *count = param_count;
    return params;
}

static AstDecl* parse_function(Parser* parser) {
    SourceLoc loc = parser->previous.loc;

    consume(parser, TOK_IDENT, "Expected function name");
    char* name = copy_token_string(&parser->previous);

    uint32_t param_count = 0;
    Parameter* params = parse_parameters(parser, &param_count);

    // Return type (optional, defaults to void)
    TypeAnnotation return_type = {0};
    if (match(parser, TOK_ARROW)) {
        return_type = parse_type(parser);
    } else {
        return_type.name = strdup("void");
        return_type.is_array = false;
    }

    AstStmt* body = parse_block(parser);

    AstDecl* decl = malloc(sizeof(AstDecl));
    decl->kind = DECL_FUNCTION;
    decl->loc = loc;
    decl->as.function.name = name;
    decl->as.function.params = params;
    decl->as.function.param_count = param_count;
    decl->as.function.return_type = return_type;
    decl->as.function.body = body;

    return decl;
}

static ToolDecl parse_tool(Parser* parser) {
    ToolDecl tool = {0};
    tool.loc = parser->previous.loc;

    consume(parser, TOK_IDENT, "Expected tool name");
    tool.name = copy_token_string(&parser->previous);

    tool.params = parse_parameters(parser, &tool.param_count);

    // Return type
    if (match(parser, TOK_ARROW)) {
        tool.return_type = parse_type(parser);
    } else {
        tool.return_type.name = strdup("void");
        tool.return_type.is_array = false;
    }

    tool.body = parse_block(parser);

    return tool;
}

static AstDecl* parse_agent(Parser* parser) {
    SourceLoc loc = parser->previous.loc;

    consume(parser, TOK_IDENT, "Expected agent name");
    char* name = copy_token_string(&parser->previous);

    consume(parser, TOK_LBRACE, "Expected '{' after agent name");

    char* model = NULL;
    char* system_prompt = NULL;
    double temperature = 0.7;  // default

    ToolDecl* tools = NULL;
    uint32_t tool_count = 0;
    uint32_t tool_capacity = 0;

    while (!check(parser, TOK_RBRACE) && !check(parser, TOK_EOF)) {
        if (match(parser, TOK_MODEL)) {
            consume(parser, TOK_STRING, "Expected model string");
            model = copy_token_string(&parser->previous);
        }
        else if (match(parser, TOK_SYSTEM)) {
            consume(parser, TOK_STRING, "Expected system prompt string");
            system_prompt = copy_token_string(&parser->previous);
        }
        else if (match(parser, TOK_TEMPERATURE)) {
            if (match(parser, TOK_INT)) {
                temperature = (double)parser->previous.value.int_val;
            } else if (match(parser, TOK_FLOAT)) {
                temperature = parser->previous.value.float_val;
            } else {
                error(parser, "Expected number for temperature");
            }
        }
        else if (match(parser, TOK_TOOL)) {
            ToolDecl tool = parse_tool(parser);
            if (tool_count >= tool_capacity) {
                tool_capacity = tool_capacity == 0 ? 4 : tool_capacity * 2;
                tools = realloc(tools, tool_capacity * sizeof(ToolDecl));
            }
            tools[tool_count++] = tool;
        }
        else {
            error(parser, "Unexpected token in agent body: %s",
                  token_type_name(parser->current.type));
            advance(parser);
        }
    }

    consume(parser, TOK_RBRACE, "Expected '}' after agent body");

    AstDecl* decl = malloc(sizeof(AstDecl));
    decl->kind = DECL_AGENT;
    decl->loc = loc;
    decl->as.agent.name = name;
    decl->as.agent.model = model;
    decl->as.agent.system_prompt = system_prompt;
    decl->as.agent.temperature = temperature;
    decl->as.agent.tools = tools;
    decl->as.agent.tool_count = tool_count;

    return decl;
}

// Parse: import "path" [as alias];
static AstDecl* parse_import(Parser* parser) {
    SourceLoc loc = parser->previous.loc;

    // Expect string path
    if (!consume(parser, TOK_STRING, "Expected module path after 'import'")) {
        return NULL;
    }

    // Extract path string (lexer already strips quotes)
    char* path = strndup(parser->previous.value.str.start,
                         parser->previous.value.str.length);

    // Optional: as alias
    char* alias = NULL;
    if (match(parser, TOK_AS)) {
        if (!consume(parser, TOK_IDENT, "Expected alias name after 'as'")) {
            free(path);
            return NULL;
        }
        alias = strndup(parser->previous.value.str.start,
                        parser->previous.value.str.length);
    }

    if (!consume(parser, TOK_SEMICOLON, "Expected ';' after import")) {
        free(path);
        free(alias);
        return NULL;
    }

    return ast_import(path, alias, loc);
}

static AstDecl* parse_declaration(Parser* parser) {
    if (match(parser, TOK_IMPORT)) {
        return parse_import(parser);
    }
    if (match(parser, TOK_AGENT)) {
        return parse_agent(parser);
    }
    if (match(parser, TOK_FN)) {
        return parse_function(parser);
    }

    error(parser, "Expected 'import', 'agent', or 'fn' at top level, got %s",
          token_type_name(parser->current.type));
    return NULL;
}

// ============================================================================
// Public API
// ============================================================================

void parser_init(Parser* parser, Lexer* lexer) {
    parser->lexer = lexer;
    parser->had_error = false;
    parser->panic_mode = false;
    parser->error_msg[0] = '\0';

    // Prime the parser
    advance(parser);
}

AstProgram* parser_parse_program(Parser* parser) {
    AstProgram* program = ast_program_new();

    while (!check(parser, TOK_EOF)) {
        AstDecl* decl = parse_declaration(parser);
        if (decl) {
            ast_program_add_decl(program, decl);
        }

        if (parser->panic_mode) {
            synchronize(parser);
        }
    }

    return program;
}

bool parser_had_error(Parser* parser) {
    return parser->had_error;
}

const char* parser_error_msg(Parser* parser) {
    return parser->error_msg;
}

SourceLoc parser_error_loc(Parser* parser) {
    return parser->error_loc;
}

AstExpr* parser_parse_expression(Parser* parser) {
    return parse_expression(parser);
}

AstStmt* parser_parse_statement(Parser* parser) {
    return parse_statement(parser);
}

AstDecl* parser_parse_declaration(Parser* parser) {
    return parse_declaration(parser);
}
