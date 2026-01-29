#ifndef VEGA_LEXER_H
#define VEGA_LEXER_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Vega Lexer
 *
 * Tokenizes Vega source code into a stream of tokens.
 */

// ============================================================================
// Token Types
// ============================================================================

typedef enum {
    // Special
    TOK_EOF = 0,
    TOK_ERROR,

    // Literals
    TOK_IDENT,          // identifier
    TOK_STRING,         // "string literal"
    TOK_INT,            // 123
    TOK_FLOAT,          // 1.23

    // Keywords
    TOK_AGENT,          // agent
    TOK_FN,             // fn
    TOK_LET,            // let
    TOK_IF,             // if
    TOK_ELSE,           // else
    TOK_RETURN,         // return
    TOK_SPAWN,          // spawn
    TOK_ASYNC,          // async
    TOK_AWAIT,          // await
    TOK_TOOL,           // tool
    TOK_MODEL,          // model
    TOK_SYSTEM,         // system
    TOK_TEMPERATURE,    // temperature
    TOK_TRUE,           // true
    TOK_FALSE,          // false
    TOK_NULL,           // null
    TOK_WHILE,          // while
    TOK_FOR,            // for
    TOK_BREAK,          // break
    TOK_CONTINUE,       // continue
    TOK_IMPORT,         // import
    TOK_AS,             // as

    // Supervision keywords
    TOK_SUPERVISED,     // supervised
    TOK_BY,             // by
    TOK_STRATEGY,       // strategy
    TOK_RESTART,        // restart
    TOK_STOP,           // stop
    TOK_ESCALATE,       // escalate
    TOK_RESTART_ALL,    // restart_all
    TOK_MAX_RESTARTS,   // max_restarts
    TOK_WINDOW,         // window

    // Result/match keywords
    TOK_MATCH,          // match
    TOK_OK,             // Ok
    TOK_ERR,            // Err

    // Operators
    TOK_PLUS,           // +
    TOK_MINUS,          // -
    TOK_STAR,           // *
    TOK_SLASH,          // /
    TOK_PERCENT,        // %
    TOK_EQ,             // =
    TOK_EQEQ,           // ==
    TOK_NE,             // !=
    TOK_LT,             // <
    TOK_LE,             // <=
    TOK_GT,             // >
    TOK_GE,             // >=
    TOK_AND,            // &&
    TOK_OR,             // ||
    TOK_NOT,            // !
    TOK_ARROW,          // ->
    TOK_MSG,            // <-
    TOK_MSG_ASYNC,      // <~
    TOK_FATARROW,       // =>
    TOK_DOT,            // .
    TOK_COLON,          // :
    TOK_COLONCOLON,     // ::

    // Delimiters
    TOK_LPAREN,         // (
    TOK_RPAREN,         // )
    TOK_LBRACE,         // {
    TOK_RBRACE,         // }
    TOK_LBRACKET,       // [
    TOK_RBRACKET,       // ]
    TOK_COMMA,          // ,
    TOK_SEMICOLON,      // ;
} TokenType;

// ============================================================================
// Source Location
// ============================================================================

typedef struct {
    const char* filename;
    uint32_t line;
    uint32_t column;
    uint32_t offset;      // Byte offset in source
} SourceLoc;

// ============================================================================
// Token
// ============================================================================

typedef struct {
    TokenType type;
    SourceLoc loc;

    // Token value (depending on type)
    union {
        struct {
            const char* start;  // Pointer into source
            uint32_t length;
        } str;                  // For IDENT, STRING
        int64_t int_val;        // For INT
        double float_val;       // For FLOAT
    } value;
} Token;

// ============================================================================
// Lexer State
// ============================================================================

typedef struct {
    const char* source;       // Source code
    const char* start;        // Start of current token
    const char* current;      // Current position
    const char* filename;     // Source filename
    uint32_t line;            // Current line (1-indexed)
    uint32_t column;          // Current column (1-indexed)
    uint32_t line_start;      // Offset of current line start

    // Error handling
    bool had_error;
    char error_msg[256];
} Lexer;

// ============================================================================
// Lexer Functions
// ============================================================================

// Initialize lexer with source code
void lexer_init(Lexer* lexer, const char* source, const char* filename);

// Get the next token
Token lexer_next_token(Lexer* lexer);

// Peek at the next token without consuming it
Token lexer_peek_token(Lexer* lexer);

// Check if lexer had an error
bool lexer_had_error(Lexer* lexer);

// Get error message
const char* lexer_error_msg(Lexer* lexer);

// ============================================================================
// Token Utilities
// ============================================================================

// Get string representation of token type
const char* token_type_name(TokenType type);

// Check if token is a keyword
bool token_is_keyword(TokenType type);

// Print token for debugging
void token_print(Token* token);

// Copy token string value to buffer (for identifiers/strings)
// Returns number of characters copied (excluding null terminator)
uint32_t token_copy_string(Token* token, char* buffer, uint32_t buffer_size);

#endif // VEGA_LEXER_H
