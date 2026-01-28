/*
 * vegac - The Vega Compiler
 *
 * Compiles Vega source code to bytecode.
 *
 * Usage:
 *   vegac input.vega              # Output to input.vgb
 *   vegac input.vega -o out.vgb   # Output to specified file
 *   vegac input.vega -S           # Output disassembly
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <libgen.h>
#include <unistd.h>
#include <sys/stat.h>

#include "lexer.h"
#include "parser.h"
#include "ast.h"
#include "sema.h"
#include "codegen.h"
#include "../common/memory.h"

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s <input.vega> [-o <output.vgb>] [-S]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -o <file>   Write output to <file>\n");
    fprintf(stderr, "  -S          Output disassembly instead of bytecode\n");
    fprintf(stderr, "  -v          Verbose output (show compilation stages)\n");
    fprintf(stderr, "  --ast       Print AST (for debugging)\n");
    fprintf(stderr, "  --tokens    Print tokens (for debugging)\n");
    fprintf(stderr, "  -h, --help  Show this help message\n");
}

static char* read_file(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        fprintf(stderr, "Error: Cannot open file '%s'\n", filename);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char* buffer = malloc(size + 1);
    if (!buffer) {
        fprintf(stderr, "Error: Out of memory\n");
        fclose(f);
        return NULL;
    }

    size_t read = fread(buffer, 1, size, f);
    buffer[read] = '\0';
    fclose(f);

    return buffer;
}

static char* change_extension(const char* filename, const char* new_ext) {
    const char* dot = strrchr(filename, '.');
    size_t base_len = dot ? (size_t)(dot - filename) : strlen(filename);

    char* result = malloc(base_len + strlen(new_ext) + 1);
    memcpy(result, filename, base_len);
    strcpy(result + base_len, new_ext);
    return result;
}

int main(int argc, char* argv[]) {
    const char* input_file = NULL;
    const char* output_file = NULL;
    bool disassemble = false;
    bool print_ast = false;
    bool print_tokens = false;
    bool verbose = false;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-o") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: -o requires an argument\n");
                return 1;
            }
            output_file = argv[++i];
        } else if (strcmp(argv[i], "-S") == 0) {
            disassemble = true;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose = true;
        } else if (strcmp(argv[i], "--ast") == 0) {
            print_ast = true;
        } else if (strcmp(argv[i], "--tokens") == 0) {
            print_tokens = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        } else {
            if (input_file) {
                fprintf(stderr, "Error: Multiple input files not supported\n");
                return 1;
            }
            input_file = argv[i];
        }
    }

    if (!input_file) {
        fprintf(stderr, "Error: No input file specified\n");
        print_usage(argv[0]);
        return 1;
    }

    // Initialize memory system
    vega_memory_init();

    // Read source file
    if (verbose) fprintf(stderr, "[1/4] Reading %s...\n", input_file);
    char* source = read_file(input_file);
    if (!source) {
        return 1;
    }

    // Lexer
    if (verbose) fprintf(stderr, "[2/4] Parsing...\n");
    Lexer lexer;
    lexer_init(&lexer, source, input_file);

    // Token debug output
    if (print_tokens) {
        printf("=== Tokens ===\n");
        Token tok;
        while ((tok = lexer_next_token(&lexer)).type != TOK_EOF) {
            token_print(&tok);
            if (tok.type == TOK_ERROR) break;
        }
        printf("==============\n\n");

        // Re-init lexer for parsing
        lexer_init(&lexer, source, input_file);
    }

    // Parse
    Parser parser;
    parser_init(&parser, &lexer);
    AstProgram* program = parser_parse_program(&parser);

    if (parser_had_error(&parser)) {
        free(source);
        ast_program_free(program);
        vega_memory_shutdown();
        return 1;
    }

    // AST debug output
    if (print_ast) {
        printf("=== AST ===\n");
        ast_print_program(program);
        printf("===========\n\n");
    }

    // Semantic analysis
    if (verbose) fprintf(stderr, "[3/4] Analyzing...\n");
    SemanticAnalyzer sema;
    sema_init(&sema);

    // Set up import search paths
    // 1. Current working directory's stdlib
    struct stat st;
    if (stat("stdlib", &st) == 0 && S_ISDIR(st.st_mode)) {
        char cwd[1024];
        if (getcwd(cwd, sizeof(cwd))) {
            char stdlib_path[1100];
            snprintf(stdlib_path, sizeof(stdlib_path), "%s/stdlib", cwd);
            sema_add_search_path(&sema, stdlib_path);
        }
    }

    // 2. VEGA_PATH environment variable
    const char* vega_path = getenv("VEGA_PATH");
    if (vega_path) {
        sema_add_search_path(&sema, vega_path);
    }

    if (!sema_analyze(&sema, program, input_file)) {
        sema_cleanup(&sema);
        free(source);
        ast_program_free(program);
        vega_memory_shutdown();
        return 1;
    }

    // Code generation
    if (verbose) fprintf(stderr, "[4/4] Generating bytecode...\n");
    CodeGen codegen;
    codegen_init(&codegen);

    // Generate code for imported modules first
    AstProgram* modules[64];
    uint32_t module_count = sema_get_module_programs(&sema, modules, 64);
    for (uint32_t i = 0; i < module_count; i++) {
        if (!codegen_generate(&codegen, modules[i])) {
            fprintf(stderr, "Error: Code generation failed for imported module: %s\n",
                    codegen_error_msg(&codegen));
            codegen_cleanup(&codegen);
            sema_cleanup(&sema);
            free(source);
            ast_program_free(program);
            vega_memory_shutdown();
            return 1;
        }
    }

    // Generate code for main program
    if (!codegen_generate(&codegen, program)) {
        fprintf(stderr, "Error: Code generation failed: %s\n",
                codegen_error_msg(&codegen));
        codegen_cleanup(&codegen);
        sema_cleanup(&sema);
        free(source);
        ast_program_free(program);
        vega_memory_shutdown();
        return 1;
    }

    sema_cleanup(&sema);

    // Output
    if (disassemble) {
        codegen_disassemble(&codegen, stdout);
    } else {
        // Determine output filename
        char* out = output_file ?
            strdup(output_file) :
            change_extension(input_file, ".vgb");

        if (!codegen_write_file(&codegen, out)) {
            fprintf(stderr, "Error: %s\n", codegen_error_msg(&codegen));
            free(out);
            codegen_cleanup(&codegen);
            free(source);
            ast_program_free(program);
            vega_memory_shutdown();
            return 1;
        }

        printf("Compiled %s -> %s\n", input_file, out);
        free(out);
    }

    // Cleanup
    codegen_cleanup(&codegen);
    free(source);
    ast_program_free(program);
    vega_memory_shutdown();

    return 0;
}
