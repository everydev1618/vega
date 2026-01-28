#ifndef VEGA_AST_H
#define VEGA_AST_H

#include "lexer.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Vega Abstract Syntax Tree
 *
 * All AST nodes use tagged unions for type safety.
 */

// Forward declarations
typedef struct AstNode AstNode;
typedef struct AstExpr AstExpr;
typedef struct AstStmt AstStmt;
typedef struct AstDecl AstDecl;

// ============================================================================
// Expression Types
// ============================================================================

typedef enum {
    EXPR_INT_LITERAL,
    EXPR_FLOAT_LITERAL,
    EXPR_STRING_LITERAL,
    EXPR_BOOL_LITERAL,
    EXPR_NULL_LITERAL,
    EXPR_ARRAY_LITERAL,
    EXPR_IDENTIFIER,
    EXPR_BINARY,
    EXPR_UNARY,
    EXPR_CALL,
    EXPR_METHOD_CALL,
    EXPR_FIELD_ACCESS,
    EXPR_INDEX,
    EXPR_SPAWN,
    EXPR_MESSAGE,       // agent <- "message"
    EXPR_AWAIT,
} AstExprKind;

typedef enum {
    BINOP_ADD,          // +
    BINOP_SUB,          // -
    BINOP_MUL,          // *
    BINOP_DIV,          // /
    BINOP_MOD,          // %
    BINOP_EQ,           // ==
    BINOP_NE,           // !=
    BINOP_LT,           // <
    BINOP_LE,           // <=
    BINOP_GT,           // >
    BINOP_GE,           // >=
    BINOP_AND,          // &&
    BINOP_OR,           // ||
} BinaryOp;

typedef enum {
    UNOP_NEG,           // -
    UNOP_NOT,           // !
} UnaryOp;

// Supervision configuration (for supervised spawn)
typedef enum {
    RESTART_STRATEGY_RESTART,
    RESTART_STRATEGY_STOP,
    RESTART_STRATEGY_ESCALATE,
    RESTART_STRATEGY_RESTART_ALL,
} AstRestartStrategy;

typedef struct {
    AstRestartStrategy strategy;
    uint32_t max_restarts;      // Default: 3
    uint32_t window_ms;         // Default: 60000 (1 minute)
} AstSupervisionConfig;

// Expression node
struct AstExpr {
    AstExprKind kind;
    SourceLoc loc;

    union {
        // Literals
        int64_t int_val;
        double float_val;
        struct {
            char* value;
            uint32_t length;
        } string_val;
        bool bool_val;

        // Array literal: [expr, expr, ...]
        struct {
            AstExpr** elements;
            uint32_t count;
        } array_literal;

        // Identifier
        struct {
            char* name;
        } ident;

        // Binary expression
        struct {
            BinaryOp op;
            AstExpr* left;
            AstExpr* right;
        } binary;

        // Unary expression
        struct {
            UnaryOp op;
            AstExpr* operand;
        } unary;

        // Function call: name(args...)
        struct {
            AstExpr* callee;        // Can be identifier or field access
            AstExpr** args;
            uint32_t arg_count;
        } call;

        // Method call: obj.method(args...)
        struct {
            AstExpr* object;
            char* method;
            AstExpr** args;
            uint32_t arg_count;
        } method_call;

        // Field access: obj.field
        struct {
            AstExpr* object;
            char* field;
        } field_access;

        // Index: arr[idx]
        struct {
            AstExpr* object;
            AstExpr* index;
        } index;

        // Spawn: spawn AgentName  or  spawn async AgentName
        //        spawn AgentName supervised by { ... }
        struct {
            char* agent_name;
            bool is_async;
            bool is_supervised;
            AstSupervisionConfig* supervision;  // NULL if not supervised
        } spawn;

        // Message: agent <- expr
        struct {
            AstExpr* target;
            AstExpr* message;
        } message;

        // Await: await expr
        struct {
            AstExpr* future;
        } await;
    } as;
};

// ============================================================================
// Statement Types
// ============================================================================

typedef enum {
    STMT_EXPR,          // Expression statement
    STMT_LET,           // Variable declaration
    STMT_ASSIGN,        // Assignment
    STMT_IF,            // If/else
    STMT_WHILE,         // While loop
    STMT_FOR,           // For loop
    STMT_RETURN,        // Return
    STMT_BREAK,
    STMT_CONTINUE,
    STMT_BLOCK,         // { ... }
} AstStmtKind;

// Type annotation
typedef struct {
    char* name;         // Type name (str, int, bool, void, etc.)
    bool is_array;      // Is this type[]?
} TypeAnnotation;

// Statement node
struct AstStmt {
    AstStmtKind kind;
    SourceLoc loc;

    union {
        // Expression statement
        struct {
            AstExpr* expr;
        } expr;

        // Let statement: let name: type = value;
        struct {
            char* name;
            TypeAnnotation* type;   // Optional type annotation
            AstExpr* init;          // Optional initializer
        } let;

        // Assignment: target = value;
        struct {
            AstExpr* target;
            AstExpr* value;
        } assign;

        // If statement
        struct {
            AstExpr* condition;
            AstStmt* then_branch;
            AstStmt* else_branch;   // Can be NULL
        } if_stmt;

        // While loop
        struct {
            AstExpr* condition;
            AstStmt* body;
        } while_stmt;

        // For loop (not in v0.1, but reserved)
        struct {
            AstStmt* init;
            AstExpr* condition;
            AstExpr* update;
            AstStmt* body;
        } for_stmt;

        // Return statement
        struct {
            AstExpr* value;         // Can be NULL
        } return_stmt;

        // Block: { statements... }
        struct {
            AstStmt** stmts;
            uint32_t stmt_count;
        } block;
    } as;
};

// ============================================================================
// Declaration Types
// ============================================================================

typedef enum {
    DECL_AGENT,
    DECL_FUNCTION,
    DECL_TOOL,
} AstDeclKind;

// Parameter
typedef struct {
    char* name;
    TypeAnnotation type;
} Parameter;

// Tool declaration (inside agent)
typedef struct {
    char* name;
    char* description;          // Optional description for Claude
    Parameter* params;
    uint32_t param_count;
    TypeAnnotation return_type;
    AstStmt* body;              // Block statement
    SourceLoc loc;
} ToolDecl;

// Agent declaration
typedef struct {
    char* name;
    char* model;                // Model string
    char* system_prompt;        // System prompt
    double temperature;         // Temperature (0.0 - 1.0)
    ToolDecl* tools;
    uint32_t tool_count;
    SourceLoc loc;
} AgentDecl;

// Function declaration
typedef struct {
    char* name;
    Parameter* params;
    uint32_t param_count;
    TypeAnnotation return_type;
    AstStmt* body;              // Block statement
    SourceLoc loc;
} FunctionDecl;

// Top-level declaration
struct AstDecl {
    AstDeclKind kind;
    SourceLoc loc;

    union {
        AgentDecl agent;
        FunctionDecl function;
        ToolDecl tool;
    } as;
};

// ============================================================================
// Program (Root)
// ============================================================================

typedef struct {
    AstDecl** decls;
    uint32_t decl_count;
} AstProgram;

// ============================================================================
// AST Construction Functions
// ============================================================================

// Expressions
AstExpr* ast_int_literal(int64_t value, SourceLoc loc);
AstExpr* ast_float_literal(double value, SourceLoc loc);
AstExpr* ast_string_literal(const char* value, uint32_t length, SourceLoc loc);
AstExpr* ast_bool_literal(bool value, SourceLoc loc);
AstExpr* ast_null_literal(SourceLoc loc);
AstExpr* ast_array_literal(AstExpr** elements, uint32_t count, SourceLoc loc);
AstExpr* ast_index(AstExpr* object, AstExpr* index, SourceLoc loc);
AstExpr* ast_identifier(const char* name, SourceLoc loc);
AstExpr* ast_binary(BinaryOp op, AstExpr* left, AstExpr* right, SourceLoc loc);
AstExpr* ast_unary(UnaryOp op, AstExpr* operand, SourceLoc loc);
AstExpr* ast_call(AstExpr* callee, AstExpr** args, uint32_t arg_count, SourceLoc loc);
AstExpr* ast_method_call(AstExpr* object, const char* method, AstExpr** args, uint32_t arg_count, SourceLoc loc);
AstExpr* ast_field_access(AstExpr* object, const char* field, SourceLoc loc);
AstExpr* ast_spawn(const char* agent_name, bool is_async, SourceLoc loc);
AstExpr* ast_spawn_supervised(const char* agent_name, AstSupervisionConfig* config, SourceLoc loc);
AstExpr* ast_message(AstExpr* target, AstExpr* message, SourceLoc loc);
AstExpr* ast_await(AstExpr* future, SourceLoc loc);

// Supervision config
AstSupervisionConfig* ast_supervision_config(AstRestartStrategy strategy, uint32_t max_restarts, uint32_t window_ms);

// Statements
AstStmt* ast_expr_stmt(AstExpr* expr, SourceLoc loc);
AstStmt* ast_let_stmt(const char* name, TypeAnnotation* type, AstExpr* init, SourceLoc loc);
AstStmt* ast_assign_stmt(AstExpr* target, AstExpr* value, SourceLoc loc);
AstStmt* ast_if_stmt(AstExpr* cond, AstStmt* then_b, AstStmt* else_b, SourceLoc loc);
AstStmt* ast_while_stmt(AstExpr* cond, AstStmt* body, SourceLoc loc);
AstStmt* ast_return_stmt(AstExpr* value, SourceLoc loc);
AstStmt* ast_break_stmt(SourceLoc loc);
AstStmt* ast_continue_stmt(SourceLoc loc);
AstStmt* ast_block_stmt(AstStmt** stmts, uint32_t count, SourceLoc loc);

// Type annotation
TypeAnnotation* ast_type_annotation(const char* name, bool is_array);

// Program
AstProgram* ast_program_new(void);
void ast_program_add_decl(AstProgram* program, AstDecl* decl);

// ============================================================================
// AST Destruction Functions
// ============================================================================

void ast_expr_free(AstExpr* expr);
void ast_stmt_free(AstStmt* stmt);
void ast_decl_free(AstDecl* decl);
void ast_program_free(AstProgram* program);
void ast_type_free(TypeAnnotation* type);

// ============================================================================
// AST Printing (for debugging)
// ============================================================================

void ast_print_expr(AstExpr* expr, int indent);
void ast_print_stmt(AstStmt* stmt, int indent);
void ast_print_decl(AstDecl* decl, int indent);
void ast_print_program(AstProgram* program);

#endif // VEGA_AST_H
