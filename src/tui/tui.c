#include "tui.h"
#include "trace.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <ctype.h>
#include <time.h>

// ============================================================================
// Color Pairs
// ============================================================================

#define COLOR_DEFAULT    1
#define COLOR_HEADER     2
#define COLOR_BORDER     3
#define COLOR_AGENT      4
#define COLOR_THINKING   5
#define COLOR_TOOL       6
#define COLOR_IDLE       7
#define COLOR_PROMPT     8
#define COLOR_HELP       9
#define COLOR_TOKEN      10
#define COLOR_ERROR      11
#define COLOR_USER_MSG   12
#define COLOR_AGENT_MSG  13
#define COLOR_PRINT      14

// ============================================================================
// Forward Declarations
// ============================================================================

static void tui_handle_resize(TuiState* tui);
static void tui_create_windows(TuiState* tui);
static void tui_destroy_windows(TuiState* tui);
static void tui_draw_header(TuiState* tui);
static void tui_draw_agents(TuiState* tui);
static void tui_draw_output(TuiState* tui);
static void tui_draw_input(TuiState* tui);
static void tui_draw_help(TuiState* tui);
static void tui_handle_key(TuiState* tui, int ch);
static void trace_callback(TraceEvent* event, void* userdata);

// ============================================================================
// Initialization
// ============================================================================

bool tui_init(TuiState* tui, VegaVM* vm) {
    memset(tui, 0, sizeof(TuiState));
    tui->vm = vm;
    tui->running = true;
    tui->needs_refresh = true;
    tui->history_pos = -1;

    // Initialize ncurses
    initscr();
    clear();
    refresh();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    nodelay(stdscr, TRUE);  // Non-blocking input
    curs_set(1);            // Show cursor

    // Initialize colors if supported
    if (has_colors()) {
        start_color();
        use_default_colors();

        init_pair(COLOR_DEFAULT,  -1, -1);
        init_pair(COLOR_HEADER,   COLOR_WHITE, COLOR_BLUE);
        init_pair(COLOR_BORDER,   COLOR_CYAN, -1);
        init_pair(COLOR_AGENT,    COLOR_GREEN, -1);
        init_pair(COLOR_THINKING, COLOR_YELLOW, -1);
        init_pair(COLOR_TOOL,     COLOR_MAGENTA, -1);
        init_pair(COLOR_IDLE,     COLOR_WHITE, -1);
        init_pair(COLOR_PROMPT,   COLOR_GREEN, -1);
        init_pair(COLOR_HELP,     COLOR_YELLOW, -1);
        init_pair(COLOR_TOKEN,    COLOR_CYAN, -1);
        init_pair(COLOR_ERROR,    COLOR_RED, -1);
        init_pair(COLOR_USER_MSG, COLOR_YELLOW, -1);
        init_pair(COLOR_AGENT_MSG,COLOR_WHITE, -1);
        init_pair(COLOR_PRINT,    COLOR_WHITE, -1);
    }

    // Get terminal size
    getmaxyx(stdscr, tui->term_height, tui->term_width);

    // Calculate left column width (about 1/3 of screen, min 25, max 40)
    tui->left_col_width = tui->term_width / 3;
    if (tui->left_col_width < 25) tui->left_col_width = 25;
    if (tui->left_col_width > 40) tui->left_col_width = 40;

    // Allocate output buffer
    tui->output_buffer = calloc(TUI_OUTPUT_BUFFER_SIZE, sizeof(OutputLine));
    if (!tui->output_buffer) {
        endwin();
        return false;
    }

    // Allocate history
    tui->history_capacity = TUI_HISTORY_SIZE;
    tui->history = calloc(tui->history_capacity, sizeof(char*));
    if (!tui->history) {
        free(tui->output_buffer);
        endwin();
        return false;
    }

    // Create windows
    tui_create_windows(tui);

    // Initialize trace system and subscribe
    trace_init();
    tui->trace_subscriber_id = trace_subscribe(trace_callback, tui);

    return true;
}

void tui_cleanup(TuiState* tui) {
    // Unsubscribe from trace events
    if (tui->trace_subscriber_id) {
        trace_unsubscribe(tui->trace_subscriber_id);
    }
    trace_shutdown();

    // Free windows
    tui_destroy_windows(tui);

    // End ncurses
    endwin();

    // Free output buffer
    if (tui->output_buffer) {
        for (uint32_t i = 0; i < TUI_OUTPUT_BUFFER_SIZE; i++) {
            free(tui->output_buffer[i].agent_name);
            free(tui->output_buffer[i].text);
        }
        free(tui->output_buffer);
    }

    // Free agent info
    for (uint32_t i = 0; i < tui->agent_count; i++) {
        free(tui->agents[i].name);
        free(tui->agents[i].model);
        free(tui->agents[i].current_tool);
    }

    // Free history
    if (tui->history) {
        for (int i = 0; i < tui->history_count; i++) {
            free(tui->history[i]);
        }
        free(tui->history);
    }

    // Free error tracking
    free(tui->last_error);
    free(tui->last_error_agent);
}

// ============================================================================
// Window Management
// ============================================================================

static void tui_create_windows(TuiState* tui) {
    int header_height = 1;
    int input_height = 3;
    int main_height = tui->term_height - header_height - input_height;
    int right_width = tui->term_width - tui->left_col_width;

    // Header (top bar)
    tui->header_win = newwin(header_height, tui->term_width, 0, 0);

    // Agents (left column)
    tui->agents_win = newwin(main_height, tui->left_col_width, header_height, 0);

    // Output (right column)
    tui->output_win = newwin(main_height, right_width, header_height, tui->left_col_width);

    // Input (bottom)
    tui->input_win = newwin(input_height, tui->term_width, tui->term_height - input_height, 0);

    // Enable scrolling for output
    scrollok(tui->output_win, TRUE);
}

static void tui_destroy_windows(TuiState* tui) {
    if (tui->header_win) delwin(tui->header_win);
    if (tui->agents_win) delwin(tui->agents_win);
    if (tui->output_win) delwin(tui->output_win);
    if (tui->input_win) delwin(tui->input_win);
}

static void tui_handle_resize(TuiState* tui) {
    int new_height, new_width;
    getmaxyx(stdscr, new_height, new_width);

    if (new_height != tui->term_height || new_width != tui->term_width) {
        tui->term_height = new_height;
        tui->term_width = new_width;

        // Recalculate left column width
        tui->left_col_width = tui->term_width / 3;
        if (tui->left_col_width < 25) tui->left_col_width = 25;
        if (tui->left_col_width > 40) tui->left_col_width = 40;

        // Recreate windows
        tui_destroy_windows(tui);
        clear();
        refresh();
        tui_create_windows(tui);
        tui->needs_refresh = true;
    }
}

// ============================================================================
// Drawing
// ============================================================================

void tui_refresh(TuiState* tui) {
    if (tui->show_help) {
        tui_draw_help(tui);
        return;
    }

    tui_draw_header(tui);
    tui_draw_agents(tui);
    tui_draw_output(tui);
    tui_draw_input(tui);

    // Position cursor in input
    wmove(tui->input_win, 1, 2 + tui->input_pos);
    wrefresh(tui->input_win);
}

static void tui_draw_header(TuiState* tui) {
    werase(tui->header_win);
    wbkgd(tui->header_win, COLOR_PAIR(COLOR_HEADER));

    // Title
    mvwprintw(tui->header_win, 0, 1, "VEGA");

    // Token counts and cost from VM budget tracking
    uint64_t in_tokens = tui->vm ? tui->vm->budget_used_input_tokens : tui->total_input_tokens;
    uint64_t out_tokens = tui->vm ? tui->vm->budget_used_output_tokens : tui->total_output_tokens;
    double cost = tui->vm ? tui->vm->budget_used_cost_usd : 0.0;

    if (in_tokens > 0 || out_tokens > 0) {
        char stats_str[96];
        snprintf(stats_str, sizeof(stats_str), "%lluk in / %lluk out | $%.4f",
                 (unsigned long long)(in_tokens / 1000),
                 (unsigned long long)(out_tokens / 1000),
                 cost);

        int stats_x = tui->term_width - strlen(stats_str) - 22;
        if (stats_x > 10) {
            wattron(tui->header_win, COLOR_PAIR(COLOR_HEADER));
            mvwprintw(tui->header_win, 0, stats_x, "%s", stats_str);
        }
    }

    // Help hint
    mvwprintw(tui->header_win, 0, tui->term_width - 18, "[F1:Help] [F10:Q]");

    wrefresh(tui->header_win);
}

static void tui_draw_agents(TuiState* tui) {
    werase(tui->agents_win);
    box(tui->agents_win, 0, 0);

    // Title
    wattron(tui->agents_win, COLOR_PAIR(COLOR_BORDER) | A_BOLD);
    mvwprintw(tui->agents_win, 0, 2, " AGENTS ");
    wattroff(tui->agents_win, COLOR_PAIR(COLOR_BORDER) | A_BOLD);

    int max_y, max_x;
    getmaxyx(tui->agents_win, max_y, max_x);
    int content_height = max_y - 2;
    (void)max_x;

    if (tui->agent_count == 0) {
        wattron(tui->agents_win, A_DIM);
        mvwprintw(tui->agents_win, 2, 2, "(no agents)");
        wattroff(tui->agents_win, A_DIM);
    } else {
        int y = 1;
        for (uint32_t i = 0; i < tui->agent_count && y < content_height - 1; i++) {
            TuiAgentInfo* agent = &tui->agents[i];
            if (!agent->active) continue;

            // Agent name
            wattron(tui->agents_win, COLOR_PAIR(COLOR_AGENT) | A_BOLD);
            mvwprintw(tui->agents_win, y, 2, "%s", agent->name);
            wattroff(tui->agents_win, COLOR_PAIR(COLOR_AGENT) | A_BOLD);
            y++;

            // Status line
            if (y < content_height) {
                switch (agent->status) {
                    case AGENT_STATUS_THINKING:
                        wattron(tui->agents_win, COLOR_PAIR(COLOR_THINKING) | A_BOLD);
                        mvwprintw(tui->agents_win, y, 4, "[thinking]");
                        wattroff(tui->agents_win, COLOR_PAIR(COLOR_THINKING) | A_BOLD);
                        wattron(tui->agents_win, A_DIM);
                        wprintw(tui->agents_win, " waiting for API response...");
                        wattroff(tui->agents_win, A_DIM);
                        break;

                    case AGENT_STATUS_TOOL_CALL:
                        wattron(tui->agents_win, COLOR_PAIR(COLOR_TOOL) | A_BOLD);
                        mvwprintw(tui->agents_win, y, 4, "[tool]");
                        wattroff(tui->agents_win, COLOR_PAIR(COLOR_TOOL) | A_BOLD);
                        if (agent->current_tool) {
                            wattron(tui->agents_win, COLOR_PAIR(COLOR_TOOL));
                            wprintw(tui->agents_win, " %s", agent->current_tool);
                            wattroff(tui->agents_win, COLOR_PAIR(COLOR_TOOL));
                        }
                        break;

                    case AGENT_STATUS_IDLE:
                    default:
                        wattron(tui->agents_win, A_DIM);
                        mvwprintw(tui->agents_win, y, 4, "[idle]");
                        wattroff(tui->agents_win, A_DIM);
                        break;
                }
                y++;
            }

            // Token usage for this agent
            if (y < content_height && (agent->total_input_tokens > 0 || agent->total_output_tokens > 0)) {
                wattron(tui->agents_win, COLOR_PAIR(COLOR_TOKEN) | A_DIM);
                mvwprintw(tui->agents_win, y, 4, "%llu in / %llu out",
                         (unsigned long long)agent->total_input_tokens,
                         (unsigned long long)agent->total_output_tokens);
                wattroff(tui->agents_win, COLOR_PAIR(COLOR_TOKEN) | A_DIM);
                y++;
            }

            y++;  // Blank line between agents
        }
    }

    // Display last error at the bottom of the panel
    if (tui->last_error) {
        int err_y = max_y - 6;  // Reserve 5 lines at bottom for error
        if (err_y > 2) {  // At least have some space to display
            // Separator
            mvwhline(tui->agents_win, err_y - 1, 1, ACS_HLINE, max_x - 2);

            // Error header
            wattron(tui->agents_win, COLOR_PAIR(COLOR_ERROR) | A_BOLD);
            if (tui->error_is_fatal) {
                mvwprintw(tui->agents_win, err_y, 2, "FATAL ERROR");
            } else {
                mvwprintw(tui->agents_win, err_y, 2, "ERROR (recoverable)");
            }
            wattroff(tui->agents_win, COLOR_PAIR(COLOR_ERROR) | A_BOLD);
            err_y++;

            // Agent context
            if (tui->last_error_agent) {
                wattron(tui->agents_win, A_DIM);
                mvwprintw(tui->agents_win, err_y, 2, "Agent: %s", tui->last_error_agent);
                wattroff(tui->agents_win, A_DIM);
                err_y++;
            }

            // Error message (truncate to fit)
            wattron(tui->agents_win, COLOR_PAIR(COLOR_ERROR));
            char truncated[256];
            snprintf(truncated, sizeof(truncated), "%.60s%s",
                    tui->last_error,
                    strlen(tui->last_error) > 60 ? "..." : "");
            mvwprintw(tui->agents_win, err_y, 2, "%s", truncated);
            wattroff(tui->agents_win, COLOR_PAIR(COLOR_ERROR));
            err_y++;

            // Recovery suggestion
            wattron(tui->agents_win, A_DIM);
            if (strstr(tui->last_error, "Budget exceeded")) {
                mvwprintw(tui->agents_win, err_y, 2, "Tip: Increase budget with --budget-cost");
            } else if (strstr(tui->last_error, "API key")) {
                mvwprintw(tui->agents_win, err_y, 2, "Tip: Set ANTHROPIC_API_KEY in ~/.vega");
            } else if (strstr(tui->last_error, "429") || strstr(tui->last_error, "rate")) {
                mvwprintw(tui->agents_win, err_y, 2, "Tip: Waiting for rate limit, will retry...");
            } else if (strstr(tui->last_error, "Circuit breaker")) {
                mvwprintw(tui->agents_win, err_y, 2, "Tip: Too many failures, circuit open for 60s");
            } else if (!tui->error_is_fatal) {
                mvwprintw(tui->agents_win, err_y, 2, "Tip: Error may resolve automatically");
            }
            wattroff(tui->agents_win, A_DIM);
        }
    }

    wrefresh(tui->agents_win);
}

static void tui_draw_output(TuiState* tui) {
    werase(tui->output_win);
    box(tui->output_win, 0, 0);

    // Title
    wattron(tui->output_win, COLOR_PAIR(COLOR_BORDER) | A_BOLD);
    mvwprintw(tui->output_win, 0, 2, " OUTPUT ");
    wattroff(tui->output_win, COLOR_PAIR(COLOR_BORDER) | A_BOLD);

    int max_y, max_x;
    getmaxyx(tui->output_win, max_y, max_x);
    int content_height = max_y - 2;
    int content_width = max_x - 4;

    if (tui->output_count == 0) {
        wattron(tui->output_win, A_DIM);
        mvwprintw(tui->output_win, 2, 2, "(no output yet)");
        wattroff(tui->output_win, A_DIM);
    } else {
        // Calculate visible range (show most recent, scrolled)
        int visible_start = 0;
        if ((int)tui->output_count > content_height) {
            visible_start = tui->output_count - content_height - tui->output_scroll;
            if (visible_start < 0) visible_start = 0;
        }

        int y = 1;
        for (int i = visible_start; i < (int)tui->output_count && y < content_height + 1; i++) {
            // Calculate actual index in ring buffer
            int idx = (tui->output_head - tui->output_count + i + TUI_OUTPUT_BUFFER_SIZE) % TUI_OUTPUT_BUFFER_SIZE;
            OutputLine* line = &tui->output_buffer[idx];

            if (!line->text) continue;

            // Determine color and prefix based on type
            int color = COLOR_DEFAULT;
            const char* prefix = "";

            switch (line->type) {
                case OUTPUT_USER_MSG:
                    color = COLOR_USER_MSG;
                    prefix = "->";
                    break;
                case OUTPUT_AGENT_MSG:
                    color = COLOR_AGENT_MSG;
                    prefix = "<-";
                    break;
                case OUTPUT_PRINT:
                    color = COLOR_PRINT;
                    break;
                case OUTPUT_ERROR:
                    color = COLOR_ERROR;
                    prefix = "!!";
                    break;
                case OUTPUT_TOOL:
                    color = COLOR_TOOL;
                    prefix = "()";
                    break;
                case OUTPUT_SYSTEM:
                    color = COLOR_BORDER;
                    prefix = "::";
                    break;
            }

            // Draw the line with agent name prefix
            wattron(tui->output_win, COLOR_PAIR(color));

            if (line->agent_name) {
                mvwprintw(tui->output_win, y, 2, "[%s] %s ", line->agent_name, prefix);
            } else if (prefix[0]) {
                mvwprintw(tui->output_win, y, 2, "%s ", prefix);
            } else {
                wmove(tui->output_win, y, 2);
            }

            // Calculate remaining width for text
            int prefix_len = line->agent_name ? strlen(line->agent_name) + 6 : (prefix[0] ? 3 : 0);
            int text_width = content_width - prefix_len;
            if (text_width < 10) text_width = 10;

            // Print text (truncate if needed)
            int text_len = strlen(line->text);
            if (text_len > text_width) {
                waddnstr(tui->output_win, line->text, text_width - 3);
                wprintw(tui->output_win, "...");
            } else {
                wprintw(tui->output_win, "%s", line->text);
            }

            wattroff(tui->output_win, COLOR_PAIR(color));
            y++;
        }
    }

    wrefresh(tui->output_win);
}

static void tui_draw_input(TuiState* tui) {
    werase(tui->input_win);
    box(tui->input_win, 0, 0);

    // Prompt
    wattron(tui->input_win, COLOR_PAIR(COLOR_PROMPT) | A_BOLD);
    mvwprintw(tui->input_win, 1, 1, ">");
    wattroff(tui->input_win, COLOR_PAIR(COLOR_PROMPT) | A_BOLD);

    // Input text
    int max_x = getmaxx(tui->input_win);
    int visible_width = max_x - 5;
    int start = 0;
    if (tui->input_pos > visible_width) {
        start = tui->input_pos - visible_width;
    }

    mvwaddnstr(tui->input_win, 1, 3, tui->input_buffer + start,
               tui->input_len - start > visible_width ? visible_width : tui->input_len - start);

    wrefresh(tui->input_win);
}

static void tui_draw_help(TuiState* tui) {
    (void)tui;
    werase(stdscr);

    int y = 2;
    int x = 4;

    attron(COLOR_PAIR(COLOR_HELP) | A_BOLD);
    mvprintw(y++, x, "Vega TUI Help");
    attroff(COLOR_PAIR(COLOR_HELP) | A_BOLD);
    y++;

    mvprintw(y++, x, "Commands:");
    mvprintw(y++, x + 2, "load <file.vgb>  - Load and run a program");
    mvprintw(y++, x + 2, "run              - Re-run loaded program");
    mvprintw(y++, x + 2, "clear            - Clear agent list");
    mvprintw(y++, x + 2, "help             - Show this help");
    mvprintw(y++, x + 2, "quit / exit      - Exit the TUI");
    y++;

    mvprintw(y++, x, "Keyboard:");
    mvprintw(y++, x + 2, "F1               - Toggle this help");
    mvprintw(y++, x + 2, "F10 / Ctrl-Q     - Quit");
    mvprintw(y++, x + 2, "Ctrl-L           - Refresh display");
    mvprintw(y++, x + 2, "Ctrl-C           - Cancel input");
    mvprintw(y++, x + 2, "Up/Down          - Command history");
    y++;

    attron(A_DIM);
    mvprintw(y++, x, "Press any key to return...");
    attroff(A_DIM);

    refresh();
}

// ============================================================================
// Input Handling
// ============================================================================

static void tui_handle_key(TuiState* tui, int ch) {
    // Help mode - any key exits
    if (tui->show_help) {
        tui->show_help = false;
        tui->needs_refresh = true;
        return;
    }

    switch (ch) {
        case KEY_F(1):
            tui->show_help = true;
            tui->needs_refresh = true;
            break;

        case KEY_F(10):
        case 17:  // Ctrl-Q
            tui->running = false;
            break;

        case KEY_UP:
            // Command history
            if (tui->history_count > 0) {
                if (tui->history_pos < 0) {
                    tui->history_pos = tui->history_count - 1;
                } else if (tui->history_pos > 0) {
                    tui->history_pos--;
                }
                strncpy(tui->input_buffer, tui->history[tui->history_pos], TUI_INPUT_BUFFER_SIZE - 1);
                tui->input_len = strlen(tui->input_buffer);
                tui->input_pos = tui->input_len;
            }
            tui->needs_refresh = true;
            break;

        case KEY_DOWN:
            // Command history
            if (tui->history_pos >= 0) {
                tui->history_pos++;
                if (tui->history_pos >= tui->history_count) {
                    tui->history_pos = -1;
                    tui->input_buffer[0] = '\0';
                    tui->input_len = 0;
                    tui->input_pos = 0;
                } else {
                    strncpy(tui->input_buffer, tui->history[tui->history_pos], TUI_INPUT_BUFFER_SIZE - 1);
                    tui->input_len = strlen(tui->input_buffer);
                    tui->input_pos = tui->input_len;
                }
            }
            tui->needs_refresh = true;
            break;

        case KEY_PPAGE:  // Page Up - scroll output up
            tui->output_scroll += 5;
            if (tui->output_scroll > (int)tui->output_count - 5) {
                tui->output_scroll = tui->output_count > 5 ? tui->output_count - 5 : 0;
            }
            tui->needs_refresh = true;
            break;

        case KEY_NPAGE:  // Page Down - scroll output down
            tui->output_scroll -= 5;
            if (tui->output_scroll < 0) tui->output_scroll = 0;
            tui->needs_refresh = true;
            break;

        case KEY_LEFT:
            if (tui->input_pos > 0) {
                tui->input_pos--;
                tui->needs_refresh = true;
            }
            break;

        case KEY_RIGHT:
            if (tui->input_pos < tui->input_len) {
                tui->input_pos++;
                tui->needs_refresh = true;
            }
            break;

        case KEY_BACKSPACE:
        case 127:
        case '\b':
            if (tui->input_pos > 0) {
                memmove(tui->input_buffer + tui->input_pos - 1,
                        tui->input_buffer + tui->input_pos,
                        tui->input_len - tui->input_pos + 1);
                tui->input_pos--;
                tui->input_len--;
                tui->needs_refresh = true;
            }
            break;

        case KEY_DC:  // Delete key
            if (tui->input_pos < tui->input_len) {
                memmove(tui->input_buffer + tui->input_pos,
                        tui->input_buffer + tui->input_pos + 1,
                        tui->input_len - tui->input_pos);
                tui->input_len--;
                tui->needs_refresh = true;
            }
            break;

        case KEY_HOME:
        case 1:  // Ctrl-A
            tui->input_pos = 0;
            tui->needs_refresh = true;
            break;

        case KEY_END:
        case 5:  // Ctrl-E
            tui->input_pos = tui->input_len;
            tui->needs_refresh = true;
            break;

        case 12:  // Ctrl-L - refresh
            clear();
            refresh();
            tui->needs_refresh = true;
            break;

        case 3:  // Ctrl-C - clear input
            tui->input_buffer[0] = '\0';
            tui->input_len = 0;
            tui->input_pos = 0;
            tui->history_pos = -1;
            tui->needs_refresh = true;
            break;

        case '\n':
        case '\r':
        case KEY_ENTER:
            if (tui->input_len > 0) {
                // Add to history
                if (tui->history_count < tui->history_capacity) {
                    tui->history[tui->history_count++] = strdup(tui->input_buffer);
                } else {
                    free(tui->history[0]);
                    memmove(tui->history, tui->history + 1, (tui->history_capacity - 1) * sizeof(char*));
                    tui->history[tui->history_capacity - 1] = strdup(tui->input_buffer);
                }

                // Process command
                tui_process_command(tui, tui->input_buffer);

                // Clear input
                tui->input_buffer[0] = '\0';
                tui->input_len = 0;
                tui->input_pos = 0;
                tui->history_pos = -1;
            }
            tui->needs_refresh = true;
            break;

        default:
            // Regular character input
            if (isprint(ch) && tui->input_len < TUI_INPUT_BUFFER_SIZE - 1) {
                memmove(tui->input_buffer + tui->input_pos + 1,
                        tui->input_buffer + tui->input_pos,
                        tui->input_len - tui->input_pos + 1);
                tui->input_buffer[tui->input_pos++] = ch;
                tui->input_len++;
                tui->needs_refresh = true;
            }
            break;
    }
}

// ============================================================================
// Main Loop
// ============================================================================

int tui_run(TuiState* tui) {
    // Force initial refresh
    tui_refresh(tui);

    while (tui->running) {
        // Check for resize
        tui_handle_resize(tui);

        // Handle input (non-blocking)
        int ch = getch();
        if (ch != ERR) {
            tui_handle_key(tui, ch);
        }

        // Step the VM if a program is running
        if (tui->program_running && tui->vm && tui->vm->running) {
            // Execute a batch of instructions, then refresh
            for (int i = 0; i < 100 && tui->vm->running; i++) {
                vm_step(tui->vm);
            }

            // Check if program finished
            if (!tui->vm->running) {
                tui->program_running = false;
            }
            tui->needs_refresh = true;
        }

        // Refresh display if needed
        if (tui->needs_refresh) {
            tui_refresh(tui);
            tui->needs_refresh = false;
        }

        // Small delay to avoid busy-waiting
        napms(tui->program_running ? 1 : 16);
    }

    return 0;
}

// ============================================================================
// Trace Callback
// ============================================================================

static void trace_callback(TraceEvent* event, void* userdata) {
    TuiState* tui = (TuiState*)userdata;
    if (!tui) return;

    const char* agent_name = event->agent_name ? event->agent_name : tui_get_agent_name(tui, event->agent_id);

    switch (event->type) {
        case TRACE_AGENT_SPAWN:
            tui_track_agent(tui, event->agent_id, event->agent_name, event->data);
            tui_add_output(tui, OUTPUT_SYSTEM, event->agent_name, "spawned");
            tui_refresh(tui);
            break;

        case TRACE_AGENT_FREE:
            tui_untrack_agent(tui, event->agent_id);
            break;

        case TRACE_MSG_SEND:
            tui_set_agent_status(tui, event->agent_id, AGENT_STATUS_THINKING, NULL);
            if (event->data) {
                char preview[100];
                snprintf(preview, sizeof(preview), "%.80s%s",
                         event->data, strlen(event->data) > 80 ? "..." : "");
                tui_add_output(tui, OUTPUT_USER_MSG, agent_name, preview);
            }
            tui_refresh(tui);
            break;

        case TRACE_MSG_RECV:
            tui_set_agent_status(tui, event->agent_id, AGENT_STATUS_IDLE, NULL);
            tui_update_agent_tokens(tui, event->agent_id, &event->tokens, event->duration_ms);
            if (event->data) {
                char preview[100];
                snprintf(preview, sizeof(preview), "%.80s%s",
                         event->data, strlen(event->data) > 80 ? "..." : "");
                tui_add_output(tui, OUTPUT_AGENT_MSG, agent_name, preview);
            }
            break;

        case TRACE_TOOL_CALL:
            tui_set_agent_status(tui, event->agent_id, AGENT_STATUS_TOOL_CALL, event->data);
            tui_add_output(tui, OUTPUT_TOOL, agent_name, event->data);
            tui_refresh(tui);
            break;

        case TRACE_TOOL_RESULT:
            // Tool finished - back to thinking
            for (uint32_t i = 0; i < tui->agent_count; i++) {
                if (tui->agents[i].agent_id == event->agent_id &&
                    tui->agents[i].status == AGENT_STATUS_TOOL_CALL) {
                    tui->agents[i].status = AGENT_STATUS_THINKING;
                    break;
                }
            }
            break;

        case TRACE_HTTP_DONE:
            tui->total_input_tokens += event->tokens.input_tokens;
            tui->total_output_tokens += event->tokens.output_tokens;
            break;

        case TRACE_ERROR:
            // Add to output
            if (event->data) {
                tui_add_output(tui, OUTPUT_ERROR, agent_name, event->data);
            }
            // Also track for agent panel error display
            free(tui->last_error);
            free(tui->last_error_agent);
            tui->last_error = event->data ? strdup(event->data) : strdup("Unknown error");
            tui->last_error_agent = event->agent_name ? strdup(event->agent_name) : NULL;
            tui->last_error_time = event->timestamp_ms;
            tui->error_is_fatal = false;
            if (event->data) {
                if (strstr(event->data, "Budget exceeded") ||
                    strstr(event->data, "API key") ||
                    strstr(event->data, "Invalid") ||
                    strstr(event->data, "authentication")) {
                    tui->error_is_fatal = true;
                }
            }
            tui_refresh(tui);
            break;

        case TRACE_PRINT:
            if (event->data) {
                tui_add_output(tui, OUTPUT_PRINT, NULL, event->data);
            }
            break;

        default:
            break;
    }

    tui->needs_refresh = true;
}

// ============================================================================
// Output Management
// ============================================================================

void tui_add_output(TuiState* tui, OutputType type, const char* agent_name, const char* text) {
    // Free old entry if overwriting
    OutputLine* line = &tui->output_buffer[tui->output_head];
    free(line->agent_name);
    free(line->text);

    line->type = type;
    line->agent_name = agent_name ? strdup(agent_name) : NULL;
    line->text = text ? strdup(text) : strdup("");

    tui->output_head = (tui->output_head + 1) % TUI_OUTPUT_BUFFER_SIZE;
    if (tui->output_count < TUI_OUTPUT_BUFFER_SIZE) {
        tui->output_count++;
    }

    // Reset scroll to show latest
    tui->output_scroll = 0;
}

const char* tui_get_agent_name(TuiState* tui, uint32_t agent_id) {
    for (uint32_t i = 0; i < tui->agent_count; i++) {
        if (tui->agents[i].agent_id == agent_id) {
            return tui->agents[i].name;
        }
    }
    return NULL;
}

// ============================================================================
// Agent Management
// ============================================================================

void tui_track_agent(TuiState* tui, uint32_t agent_id, const char* name, const char* model) {
    // Check if already tracking
    for (uint32_t i = 0; i < tui->agent_count; i++) {
        if (tui->agents[i].agent_id == agent_id) {
            tui->agents[i].active = true;
            return;
        }
    }

    if (tui->agent_count >= TUI_MAX_AGENTS) return;

    TuiAgentInfo* agent = &tui->agents[tui->agent_count];
    memset(agent, 0, sizeof(TuiAgentInfo));
    agent->agent_id = agent_id;
    agent->name = name ? strdup(name) : strdup("unnamed");
    agent->active = true;
    agent->status = AGENT_STATUS_IDLE;

    // Extract model from JSON if provided
    if (model && strstr(model, "\"model\":")) {
        const char* start = strstr(model, "\"model\":\"");
        if (start) {
            start += 9;
            const char* end = strchr(start, '"');
            if (end) {
                agent->model = strndup(start, end - start);
            }
        }
    }
    if (!agent->model) {
        agent->model = model ? strdup(model) : strdup("unknown");
    }

    tui->agent_count++;
}

void tui_untrack_agent(TuiState* tui, uint32_t agent_id) {
    for (uint32_t i = 0; i < tui->agent_count; i++) {
        if (tui->agents[i].agent_id == agent_id) {
            tui->agents[i].active = false;
            return;
        }
    }
}

void tui_set_agent_status(TuiState* tui, uint32_t agent_id, AgentStatus status, const char* tool_name) {
    for (uint32_t i = 0; i < tui->agent_count; i++) {
        if (tui->agents[i].agent_id == agent_id) {
            tui->agents[i].status = status;
            free(tui->agents[i].current_tool);
            tui->agents[i].current_tool = tool_name ? strdup(tool_name) : NULL;
            return;
        }
    }
}

void tui_update_agent_tokens(TuiState* tui, uint32_t agent_id, TokenUsage* tokens, uint64_t duration_ms) {
    for (uint32_t i = 0; i < tui->agent_count; i++) {
        if (tui->agents[i].agent_id == agent_id) {
            tui->agents[i].total_input_tokens += tokens->input_tokens;
            tui->agents[i].total_output_tokens += tokens->output_tokens;
            tui->agents[i].total_duration_ms += duration_ms;
            break;
        }
    }

    tui->total_input_tokens += tokens->input_tokens;
    tui->total_output_tokens += tokens->output_tokens;
}

// ============================================================================
// Commands
// ============================================================================

bool tui_process_command(TuiState* tui, const char* command) {
    // Skip leading whitespace
    while (*command && isspace(*command)) command++;

    if (strlen(command) == 0) return true;

    // Parse command
    char cmd[64];
    char arg[512];
    arg[0] = '\0';
    sscanf(command, "%63s %511[^\n]", cmd, arg);

    if (strcmp(cmd, "quit") == 0 || strcmp(cmd, "exit") == 0 || strcmp(cmd, "q") == 0) {
        tui->running = false;
        return false;
    }

    if (strcmp(cmd, "help") == 0 || strcmp(cmd, "?") == 0) {
        tui->show_help = true;
        return true;
    }

    if (strcmp(cmd, "clear") == 0) {
        // Clear agents
        for (uint32_t i = 0; i < tui->agent_count; i++) {
            free(tui->agents[i].name);
            free(tui->agents[i].model);
            free(tui->agents[i].current_tool);
        }
        tui->agent_count = 0;
        tui->total_input_tokens = 0;
        tui->total_output_tokens = 0;
        return true;
    }

    if (strcmp(cmd, "load") == 0 && strlen(arg) > 0) {
        return tui_load_program(tui, arg);
    }

    if (strcmp(cmd, "run") == 0) {
        if (tui->vm && tui->vm->code_size > 0) {
            // Find and set up main function
            int main_id = vm_find_function(tui->vm, "main");
            if (main_id < 0) {
                return true;
            }

            FunctionDef* main_fn = &tui->vm->functions[main_id];
            tui->vm->ip = main_fn->code_offset;
            tui->vm->sp = 0;
            tui->vm->frame_count = 0;
            tui->vm->running = true;
            tui->vm->had_error = false;

            // Reserve space for main's locals
            while (tui->vm->sp < main_fn->local_count) {
                vm_push(tui->vm, value_null());
            }

            tui->program_running = true;
        }
        return true;
    }

    return true;
}

bool tui_load_program(TuiState* tui, const char* path) {
    if (!vm_load_file(tui->vm, path)) {
        return false;
    }

    // Set up VM to execute main()
    int main_id = vm_find_function(tui->vm, "main");
    if (main_id < 0) {
        return false;
    }

    FunctionDef* main_fn = &tui->vm->functions[main_id];
    tui->vm->ip = main_fn->code_offset;
    tui->vm->sp = 0;
    tui->vm->frame_count = 0;
    tui->vm->running = true;
    tui->vm->had_error = false;

    // Reserve space for main's locals
    while (tui->vm->sp < main_fn->local_count) {
        vm_push(tui->vm, value_null());
    }

    // Start execution
    tui->program_running = true;

    return true;
}
