/*
 * vega - The Vega VM
 *
 * Executes Vega bytecode files.
 *
 * Usage:
 *   vega program.vgb
 *   vega init [project-name]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

#include "vm.h"
#include "http.h"
#include "../common/memory.h"

static void print_usage(const char* prog) {
    fprintf(stderr, "Usage: %s <program.vgb> [options]\n", prog);
    fprintf(stderr, "       %s init [project-name]\n", prog);
    fprintf(stderr, "\n");
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  init [name]  Create a new Vega project\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --debug     Print debug information\n");
    fprintf(stderr, "  -h, --help  Show this help message\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Environment:\n");
    fprintf(stderr, "  ANTHROPIC_API_KEY  Required for agent operations\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Config:\n");
    fprintf(stderr, "  ~/.vega            Config file (ANTHROPIC_API_KEY=sk-...)\n");
}

/*
 * Project initialization
 */

static const char* CLAUDE_MD_CONTENT =
"# Vega Project\n"
"\n"
"This project uses **Vega**, a language for building AI agent systems.\n"
"\n"
"## Quick Reference\n"
"\n"
"```bash\n"
"vegac program.vega -o program.vgb   # Compile\n"
"vega program.vgb                     # Run (needs ~/.vega or ANTHROPIC_API_KEY)\n"
"```\n"
"\n"
"## Syntax Cheatsheet\n"
"\n"
"```vega\n"
"// Types: int, float, str, bool, null, type[]\n"
"\n"
"// Variables\n"
"let x = 42;\n"
"let name: str = \"Alice\";\n"
"\n"
"// Functions\n"
"fn add(a: int, b: int) -> int {\n"
"    return a + b;\n"
"}\n"
"\n"
"// Agents\n"
"agent Helper {\n"
"    model \"claude-sonnet-4-20250514\"\n"
"    system \"You are helpful.\"\n"
"    temperature 0.7\n"
"\n"
"    tool read_file(path: str) -> str {\n"
"        return file::read(path);\n"
"    }\n"
"}\n"
"\n"
"// Main entry point\n"
"fn main() {\n"
"    let helper = spawn Helper;\n"
"    let response = helper <- \"Hello!\";\n"
"    print(response);\n"
"\n"
"    // String methods\n"
"    if response.has(\"keyword\") {\n"
"        print(\"Found it\");\n"
"    }\n"
"\n"
"    // Loops\n"
"    let i = 0;\n"
"    while i < 5 {\n"
"        print(i);\n"
"        i = i + 1;\n"
"    }\n"
"\n"
"    for let j = 0; j < 5; j {\n"
"        print(j);\n"
"        j = j + 1;\n"
"    }\n"
"}\n"
"```\n"
"\n"
"## Key Points\n"
"\n"
"- Agents wrap Claude API with config (model, system prompt, temperature, tools)\n"
"- `spawn Agent` creates an agent handle\n"
"- `agent <- \"message\"` sends a message and waits for response\n"
"- Agent conversations persist (maintains message history)\n"
"- Use `.has(\"substring\")` to check agent responses\n"
"- `+` concatenates strings\n"
"- `print()` is the output function\n";

static const char* HELLO_VEGA_CONTENT =
"// A simple Vega program with an AI agent\n"
"\n"
"agent Assistant {\n"
"    model \"claude-sonnet-4-20250514\"\n"
"    system \"You are a helpful assistant. Keep responses concise.\"\n"
"    temperature 0.7\n"
"}\n"
"\n"
"fn main() {\n"
"    let assistant = spawn Assistant;\n"
"    let response = assistant <- \"Hello! What can you help me with today?\";\n"
"    print(response);\n"
"}\n";

static const char* GITIGNORE_CONTENT =
"# Compiled bytecode\n"
"*.vgb\n"
"\n"
"# Build directory\n"
"build/\n"
"\n"
"# Editor files\n"
".vscode/\n"
".idea/\n"
"*.swp\n"
"*.swo\n"
"*~\n"
"\n"
"# OS files\n"
".DS_Store\n"
"Thumbs.db\n";

static bool write_file(const char* path, const char* content) {
    FILE* f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "Error: Cannot create %s: %s\n", path, strerror(errno));
        return false;
    }
    fputs(content, f);
    fclose(f);
    return true;
}

static bool create_directory(const char* path) {
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "Error: Cannot create directory %s: %s\n", path, strerror(errno));
        return false;
    }
    return true;
}

static int cmd_init(int argc, char* argv[]) {
    const char* project_name = NULL;

    // Parse init arguments
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: vega init [project-name]\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "Creates a new Vega project with:\n");
            fprintf(stderr, "  - CLAUDE.md    Language reference for AI assistants\n");
            fprintf(stderr, "  - hello.vega   Starter program\n");
            fprintf(stderr, "  - .gitignore   Git ignore file\n");
            fprintf(stderr, "\n");
            fprintf(stderr, "If no project name is given, initializes in current directory.\n");
            return 0;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Error: Unknown option '%s'\n", argv[i]);
            return 1;
        } else {
            if (project_name) {
                fprintf(stderr, "Error: Multiple project names not supported\n");
                return 1;
            }
            project_name = argv[i];
        }
    }

    char claude_path[512];
    char hello_path[512];
    char gitignore_path[512];

    if (project_name) {
        // Create project directory
        if (!create_directory(project_name)) {
            return 1;
        }
        snprintf(claude_path, sizeof(claude_path), "%s/CLAUDE.md", project_name);
        snprintf(hello_path, sizeof(hello_path), "%s/hello.vega", project_name);
        snprintf(gitignore_path, sizeof(gitignore_path), "%s/.gitignore", project_name);
    } else {
        // Initialize in current directory
        strcpy(claude_path, "CLAUDE.md");
        strcpy(hello_path, "hello.vega");
        strcpy(gitignore_path, ".gitignore");
    }

    // Create files
    printf("Creating Vega project%s%s...\n",
           project_name ? " '" : "",
           project_name ? project_name : "");

    if (!write_file(claude_path, CLAUDE_MD_CONTENT)) return 1;
    printf("  Created %s\n", claude_path);

    if (!write_file(hello_path, HELLO_VEGA_CONTENT)) return 1;
    printf("  Created %s\n", hello_path);

    if (!write_file(gitignore_path, GITIGNORE_CONTENT)) return 1;
    printf("  Created %s\n", gitignore_path);

    printf("\nDone! To get started:\n");
    if (project_name) {
        printf("  cd %s\n", project_name);
    }
    printf("  echo 'ANTHROPIC_API_KEY=your-key-here' >> ~/.vega\n");
    printf("  vegac hello.vega -o hello.vgb\n");
    printf("  vega hello.vgb\n");

    return 0;
}

int main(int argc, char* argv[]) {
    // Check for subcommands first
    if (argc >= 2 && strcmp(argv[1], "init") == 0) {
        return cmd_init(argc, argv);
    }

    const char* input_file = NULL;
    bool debug = false;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "--debug") == 0) {
            debug = true;
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

    // Note: API key check happens in vm_init() which checks both
    // environment variable and ~/.vega config file

    // Initialize subsystems
    vega_memory_init();

    if (!http_init()) {
        fprintf(stderr, "Error: Failed to initialize HTTP client\n");
        vega_memory_shutdown();
        return 1;
    }

    // Initialize VM
    VegaVM vm;
    vm_init(&vm);

    // Load bytecode
    if (!vm_load_file(&vm, input_file)) {
        fprintf(stderr, "Error: %s\n", vm_error_msg(&vm));
        vm_free(&vm);
        http_cleanup();
        vega_memory_shutdown();
        return 1;
    }

    if (debug) {
        printf("=== Loaded %s ===\n", input_file);
        printf("Functions: %u\n", vm.func_count);
        printf("Agents: %u\n", vm.agent_count);
        printf("Constants: %u bytes\n", vm.const_size);
        printf("Code: %u bytes\n", vm.code_size);
        printf("==================\n\n");
    }

    // Run
    bool success = vm_run(&vm);

    if (!success) {
        fprintf(stderr, "Runtime error: %s\n", vm_error_msg(&vm));
    }

    if (debug) {
        printf("\n=== Execution complete ===\n");
        vega_memory_print_stats();
    }

    // Cleanup
    vm_free(&vm);
    http_cleanup();
    vega_memory_shutdown();

    return success ? 0 : 1;
}
