#include "lexer.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// ============================================================================
// Keyword Table
// ============================================================================

typedef struct {
    const char* name;
    TokenType type;
} Keyword;

static const Keyword keywords[] = {
    {"agent",       TOK_AGENT},
    {"fn",          TOK_FN},
    {"let",         TOK_LET},
    {"if",          TOK_IF},
    {"else",        TOK_ELSE},
    {"return",      TOK_RETURN},
    {"spawn",       TOK_SPAWN},
    {"async",       TOK_ASYNC},
    {"await",       TOK_AWAIT},
    {"tool",        TOK_TOOL},
    {"model",       TOK_MODEL},
    {"system",      TOK_SYSTEM},
    {"temperature", TOK_TEMPERATURE},
    {"true",        TOK_TRUE},
    {"false",       TOK_FALSE},
    {"null",        TOK_NULL},
    {"while",       TOK_WHILE},
    {"for",         TOK_FOR},
    {"break",       TOK_BREAK},
    {"continue",    TOK_CONTINUE},
    {"import",      TOK_IMPORT},
    {"as",          TOK_AS},
    // Supervision keywords
    {"supervised",  TOK_SUPERVISED},
    {"by",          TOK_BY},
    {"strategy",    TOK_STRATEGY},
    {"restart",     TOK_RESTART},
    {"stop",        TOK_STOP},
    {"escalate",    TOK_ESCALATE},
    {"restart_all", TOK_RESTART_ALL},
    {"max_restarts", TOK_MAX_RESTARTS},
    {"window",      TOK_WINDOW},
    // Result/match keywords
    {"match",       TOK_MATCH},
    {"Ok",          TOK_OK},
    {"Err",         TOK_ERR},
    {NULL,          TOK_EOF}
};

// ============================================================================
// Helper Functions
// ============================================================================

static bool is_at_end(Lexer* lexer) {
    return *lexer->current == '\0';
}

static char peek(Lexer* lexer) {
    return *lexer->current;
}

static char peek_next(Lexer* lexer) {
    if (is_at_end(lexer)) return '\0';
    return lexer->current[1];
}

static char advance(Lexer* lexer) {
    char c = *lexer->current;
    lexer->current++;
    lexer->column++;
    return c;
}

static bool match(Lexer* lexer, char expected) {
    if (is_at_end(lexer)) return false;
    if (*lexer->current != expected) return false;
    lexer->current++;
    lexer->column++;
    return true;
}

static void skip_whitespace(Lexer* lexer) {
    for (;;) {
        char c = peek(lexer);
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance(lexer);
                break;
            case '\n':
                lexer->line++;
                lexer->column = 0;
                lexer->line_start = (uint32_t)(lexer->current - lexer->source + 1);
                advance(lexer);
                break;
            case '/':
                if (peek_next(lexer) == '/') {
                    // Line comment
                    while (peek(lexer) != '\n' && !is_at_end(lexer)) {
                        advance(lexer);
                    }
                } else if (peek_next(lexer) == '*') {
                    // Block comment
                    advance(lexer); // consume /
                    advance(lexer); // consume *
                    while (!is_at_end(lexer)) {
                        if (peek(lexer) == '*' && peek_next(lexer) == '/') {
                            advance(lexer);
                            advance(lexer);
                            break;
                        }
                        if (peek(lexer) == '\n') {
                            lexer->line++;
                            lexer->column = 0;
                            lexer->line_start = (uint32_t)(lexer->current - lexer->source + 1);
                        }
                        advance(lexer);
                    }
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

static Token make_token(Lexer* lexer, TokenType type) {
    Token token;
    token.type = type;
    token.loc.filename = lexer->filename;
    token.loc.line = lexer->line;
    token.loc.column = (uint32_t)(lexer->start - lexer->source - lexer->line_start + 1);
    token.loc.offset = (uint32_t)(lexer->start - lexer->source);
    token.value.str.start = lexer->start;
    token.value.str.length = (uint32_t)(lexer->current - lexer->start);
    return token;
}

static Token error_token(Lexer* lexer, const char* message) {
    Token token;
    token.type = TOK_ERROR;
    token.loc.filename = lexer->filename;
    token.loc.line = lexer->line;
    token.loc.column = (uint32_t)(lexer->current - lexer->source - lexer->line_start + 1);
    token.loc.offset = (uint32_t)(lexer->current - lexer->source);
    token.value.str.start = message;
    token.value.str.length = (uint32_t)strlen(message);

    lexer->had_error = true;
    snprintf(lexer->error_msg, sizeof(lexer->error_msg), "%s", message);

    return token;
}

// ============================================================================
// Token Scanning
// ============================================================================

static TokenType check_keyword(const char* start, uint32_t length) {
    for (const Keyword* kw = keywords; kw->name != NULL; kw++) {
        if (strlen(kw->name) == length && memcmp(start, kw->name, length) == 0) {
            return kw->type;
        }
    }
    return TOK_IDENT;
}

static Token scan_identifier(Lexer* lexer) {
    while (isalnum(peek(lexer)) || peek(lexer) == '_') {
        advance(lexer);
    }

    uint32_t length = (uint32_t)(lexer->current - lexer->start);
    TokenType type = check_keyword(lexer->start, length);
    return make_token(lexer, type);
}

static Token scan_number(Lexer* lexer) {
    while (isdigit(peek(lexer))) {
        advance(lexer);
    }

    // Check for decimal
    if (peek(lexer) == '.' && isdigit(peek_next(lexer))) {
        advance(lexer); // consume '.'
        while (isdigit(peek(lexer))) {
            advance(lexer);
        }
        Token token = make_token(lexer, TOK_FLOAT);
        // Parse float value
        char buffer[64];
        uint32_t len = (uint32_t)(lexer->current - lexer->start);
        if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
        memcpy(buffer, lexer->start, len);
        buffer[len] = '\0';
        token.value.float_val = strtod(buffer, NULL);
        return token;
    }

    Token token = make_token(lexer, TOK_INT);
    // Parse int value
    char buffer[64];
    uint32_t len = (uint32_t)(lexer->current - lexer->start);
    if (len >= sizeof(buffer)) len = sizeof(buffer) - 1;
    memcpy(buffer, lexer->start, len);
    buffer[len] = '\0';
    token.value.int_val = strtoll(buffer, NULL, 10);
    return token;
}

static Token scan_string(Lexer* lexer) {
    // Opening quote already consumed
    while (peek(lexer) != '"' && !is_at_end(lexer)) {
        if (peek(lexer) == '\n') {
            return error_token(lexer, "Unterminated string (newline in string literal)");
        }
        if (peek(lexer) == '\\' && peek_next(lexer) != '\0') {
            advance(lexer); // skip backslash
        }
        advance(lexer);
    }

    if (is_at_end(lexer)) {
        return error_token(lexer, "Unterminated string");
    }

    advance(lexer); // closing quote

    // The token value points to the string content (without quotes)
    Token token = make_token(lexer, TOK_STRING);
    token.value.str.start = lexer->start + 1;  // skip opening quote
    token.value.str.length = (uint32_t)(lexer->current - lexer->start - 2);  // exclude quotes
    return token;
}

// ============================================================================
// Public API
// ============================================================================

void lexer_init(Lexer* lexer, const char* source, const char* filename) {
    lexer->source = source;
    lexer->start = source;
    lexer->current = source;
    lexer->filename = filename ? filename : "<input>";
    lexer->line = 1;
    lexer->column = 1;
    lexer->line_start = 0;
    lexer->had_error = false;
    lexer->error_msg[0] = '\0';
}

Token lexer_next_token(Lexer* lexer) {
    skip_whitespace(lexer);
    lexer->start = lexer->current;

    if (is_at_end(lexer)) {
        return make_token(lexer, TOK_EOF);
    }

    char c = advance(lexer);

    // Identifiers
    if (isalpha(c) || c == '_') {
        return scan_identifier(lexer);
    }

    // Numbers
    if (isdigit(c)) {
        return scan_number(lexer);
    }

    // Strings
    if (c == '"') {
        return scan_string(lexer);
    }

    // Operators and punctuation
    switch (c) {
        case '(': return make_token(lexer, TOK_LPAREN);
        case ')': return make_token(lexer, TOK_RPAREN);
        case '{': return make_token(lexer, TOK_LBRACE);
        case '}': return make_token(lexer, TOK_RBRACE);
        case '[': return make_token(lexer, TOK_LBRACKET);
        case ']': return make_token(lexer, TOK_RBRACKET);
        case ',': return make_token(lexer, TOK_COMMA);
        case ';': return make_token(lexer, TOK_SEMICOLON);
        case '.': return make_token(lexer, TOK_DOT);
        case '+': return make_token(lexer, TOK_PLUS);
        case '*': return make_token(lexer, TOK_STAR);
        case '/': return make_token(lexer, TOK_SLASH);
        case '%': return make_token(lexer, TOK_PERCENT);

        case '-':
            if (match(lexer, '>')) return make_token(lexer, TOK_ARROW);
            return make_token(lexer, TOK_MINUS);

        case '<':
            if (match(lexer, '-')) return make_token(lexer, TOK_MSG);
            if (match(lexer, '~')) return make_token(lexer, TOK_MSG_ASYNC);
            if (match(lexer, '=')) return make_token(lexer, TOK_LE);
            return make_token(lexer, TOK_LT);

        case '>':
            if (match(lexer, '=')) return make_token(lexer, TOK_GE);
            return make_token(lexer, TOK_GT);

        case '=':
            if (match(lexer, '=')) return make_token(lexer, TOK_EQEQ);
            if (match(lexer, '>')) return make_token(lexer, TOK_FATARROW);
            return make_token(lexer, TOK_EQ);

        case '!':
            if (match(lexer, '=')) return make_token(lexer, TOK_NE);
            return make_token(lexer, TOK_NOT);

        case '&':
            if (match(lexer, '&')) return make_token(lexer, TOK_AND);
            return error_token(lexer, "Expected '&&'");

        case '|':
            if (match(lexer, '|')) return make_token(lexer, TOK_OR);
            return error_token(lexer, "Expected '||'");

        case ':':
            if (match(lexer, ':')) return make_token(lexer, TOK_COLONCOLON);
            return make_token(lexer, TOK_COLON);
    }

    return error_token(lexer, "Unexpected character");
}

Token lexer_peek_token(Lexer* lexer) {
    // Save state
    const char* saved_start = lexer->start;
    const char* saved_current = lexer->current;
    uint32_t saved_line = lexer->line;
    uint32_t saved_column = lexer->column;
    uint32_t saved_line_start = lexer->line_start;

    Token token = lexer_next_token(lexer);

    // Restore state
    lexer->start = saved_start;
    lexer->current = saved_current;
    lexer->line = saved_line;
    lexer->column = saved_column;
    lexer->line_start = saved_line_start;

    return token;
}

bool lexer_had_error(Lexer* lexer) {
    return lexer->had_error;
}

const char* lexer_error_msg(Lexer* lexer) {
    return lexer->error_msg;
}

// ============================================================================
// Token Utilities
// ============================================================================

const char* token_type_name(TokenType type) {
    switch (type) {
        case TOK_EOF:         return "EOF";
        case TOK_ERROR:       return "ERROR";
        case TOK_IDENT:       return "IDENT";
        case TOK_STRING:      return "STRING";
        case TOK_INT:         return "INT";
        case TOK_FLOAT:       return "FLOAT";
        case TOK_AGENT:       return "AGENT";
        case TOK_FN:          return "FN";
        case TOK_LET:         return "LET";
        case TOK_IF:          return "IF";
        case TOK_ELSE:        return "ELSE";
        case TOK_RETURN:      return "RETURN";
        case TOK_SPAWN:       return "SPAWN";
        case TOK_ASYNC:       return "ASYNC";
        case TOK_AWAIT:       return "AWAIT";
        case TOK_TOOL:        return "TOOL";
        case TOK_MODEL:       return "MODEL";
        case TOK_SYSTEM:      return "SYSTEM";
        case TOK_TEMPERATURE: return "TEMPERATURE";
        case TOK_TRUE:        return "TRUE";
        case TOK_FALSE:       return "FALSE";
        case TOK_NULL:        return "NULL";
        case TOK_WHILE:       return "WHILE";
        case TOK_FOR:         return "FOR";
        case TOK_BREAK:       return "BREAK";
        case TOK_CONTINUE:    return "CONTINUE";
        case TOK_IMPORT:      return "IMPORT";
        case TOK_AS:          return "AS";
        case TOK_SUPERVISED:  return "SUPERVISED";
        case TOK_BY:          return "BY";
        case TOK_STRATEGY:    return "STRATEGY";
        case TOK_RESTART:     return "RESTART";
        case TOK_STOP:        return "STOP";
        case TOK_ESCALATE:    return "ESCALATE";
        case TOK_RESTART_ALL: return "RESTART_ALL";
        case TOK_MAX_RESTARTS: return "MAX_RESTARTS";
        case TOK_WINDOW:      return "WINDOW";
        case TOK_PLUS:        return "PLUS";
        case TOK_MINUS:       return "MINUS";
        case TOK_STAR:        return "STAR";
        case TOK_SLASH:       return "SLASH";
        case TOK_PERCENT:     return "PERCENT";
        case TOK_EQ:          return "EQ";
        case TOK_EQEQ:        return "EQEQ";
        case TOK_NE:          return "NE";
        case TOK_LT:          return "LT";
        case TOK_LE:          return "LE";
        case TOK_GT:          return "GT";
        case TOK_GE:          return "GE";
        case TOK_AND:         return "AND";
        case TOK_OR:          return "OR";
        case TOK_NOT:         return "NOT";
        case TOK_ARROW:       return "ARROW";
        case TOK_MSG:         return "MSG";
        case TOK_MSG_ASYNC:   return "MSG_ASYNC";
        case TOK_DOT:         return "DOT";
        case TOK_COLON:       return "COLON";
        case TOK_COLONCOLON:  return "COLONCOLON";
        case TOK_LPAREN:      return "LPAREN";
        case TOK_RPAREN:      return "RPAREN";
        case TOK_LBRACE:      return "LBRACE";
        case TOK_RBRACE:      return "RBRACE";
        case TOK_LBRACKET:    return "LBRACKET";
        case TOK_RBRACKET:    return "RBRACKET";
        case TOK_COMMA:       return "COMMA";
        case TOK_SEMICOLON:   return "SEMICOLON";
        default:              return "UNKNOWN";
    }
}

bool token_is_keyword(TokenType type) {
    return type >= TOK_AGENT && type <= TOK_WINDOW;
}

void token_print(Token* token) {
    printf("Token(%s", token_type_name(token->type));

    if (token->type == TOK_IDENT || token->type == TOK_STRING) {
        printf(", \"");
        for (uint32_t i = 0; i < token->value.str.length; i++) {
            putchar(token->value.str.start[i]);
        }
        printf("\"");
    } else if (token->type == TOK_INT) {
        printf(", %lld", (long long)token->value.int_val);
    } else if (token->type == TOK_FLOAT) {
        printf(", %f", token->value.float_val);
    }

    printf(") at %s:%u:%u\n", token->loc.filename, token->loc.line, token->loc.column);
}

uint32_t token_copy_string(Token* token, char* buffer, uint32_t buffer_size) {
    if (buffer_size == 0) return 0;

    uint32_t copy_len = token->value.str.length;
    if (copy_len >= buffer_size) {
        copy_len = buffer_size - 1;
    }

    memcpy(buffer, token->value.str.start, copy_len);
    buffer[copy_len] = '\0';
    return copy_len;
}
