#ifndef VEGA_PARSER_H
#define VEGA_PARSER_H

#include "lexer.h"
#include "ast.h"

/*
 * Vega Parser
 *
 * Recursive descent parser that produces an AST from tokens.
 */

// ============================================================================
// Parser State
// ============================================================================

typedef struct {
    Lexer* lexer;
    Token current;
    Token previous;

    bool had_error;
    bool panic_mode;

    // Error reporting
    char error_msg[512];
    SourceLoc error_loc;
} Parser;

// ============================================================================
// Parser API
// ============================================================================

// Initialize parser with lexer
void parser_init(Parser* parser, Lexer* lexer);

// Parse entire program
AstProgram* parser_parse_program(Parser* parser);

// Check for errors
bool parser_had_error(Parser* parser);
const char* parser_error_msg(Parser* parser);
SourceLoc parser_error_loc(Parser* parser);

// Parse individual constructs (for testing)
AstExpr* parser_parse_expression(Parser* parser);
AstStmt* parser_parse_statement(Parser* parser);
AstDecl* parser_parse_declaration(Parser* parser);

#endif // VEGA_PARSER_H
