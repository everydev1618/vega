/*
 * Vega TUI Main
 *
 * Entry point for the TUI mode.
 * Usage: vega tui [program.vgb]
 */

#include "tui.h"
#include "trace.h"
#include "../vm/vm.h"
#include "../vm/http.h"
#include "../common/memory.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_tui_usage(void) {
    fprintf(stderr, "Usage: vega tui [options] [program.vgb]\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  -h, --help    Show this help message\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "If a program is specified, it will be loaded and run automatically.\n");
    fprintf(stderr, "Otherwise, the TUI starts in interactive mode.\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Keys:\n");
    fprintf(stderr, "  F1            Show help\n");
    fprintf(stderr, "  F10 / Ctrl-Q  Quit\n");
    fprintf(stderr, "  Up/Down       Command history\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  load FILE     Load a .vgb file\n");
    fprintf(stderr, "  run           Run the loaded program\n");
    fprintf(stderr, "  help          Show commands\n");
    fprintf(stderr, "  quit          Exit\n");
}

int tui_main(int argc, char* argv[]) {
    const char* input_file = NULL;

    // Parse arguments (argv[0] is "tui", start from 1)
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_tui_usage();
            return 0;
        } else if (argv[i][0] != '-') {
            input_file = argv[i];
        }
    }

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

    // Load program if specified
    bool loaded = false;
    if (input_file) {
        if (vm_load_file(&vm, input_file)) {
            loaded = true;
        } else {
            fprintf(stderr, "Warning: Failed to load %s: %s\n",
                    input_file, vm_error_msg(&vm));
        }
    }

    // Initialize TUI
    TuiState tui;
    if (!tui_init(&tui, &vm)) {
        fprintf(stderr, "Error: Failed to initialize TUI\n");
        vm_free(&vm);
        http_cleanup();
        vega_memory_shutdown();
        return 1;
    }

    // Start execution if program was loaded
    if (loaded) {
        // Set up VM to execute main()
        int main_id = vm_find_function(&vm, "main");
        if (main_id >= 0) {
            FunctionDef* main_fn = &vm.functions[main_id];
            vm.ip = main_fn->code_offset;
            vm.sp = 0;
            vm.frame_count = 0;
            vm.running = true;
            vm.had_error = false;

            // Reserve space for main's locals
            while (vm.sp < main_fn->local_count) {
                vm_push(&vm, value_null());
            }

            tui.program_running = true;
        }
    }

    // Run TUI main loop
    int exit_code = tui_run(&tui);

    // Cleanup
    tui_cleanup(&tui);
    vm_free(&vm);
    http_cleanup();
    vega_memory_shutdown();

    return exit_code;
}
