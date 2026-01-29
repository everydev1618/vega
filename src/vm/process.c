#include "process.h"
#include "vm.h"
#include "agent.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

// ============================================================================
// Time Helpers
// ============================================================================

static uint64_t current_time_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

// ============================================================================
// Process Lifecycle
// ============================================================================

VegaProcess* process_create(VegaVM* vm, uint32_t parent_pid) {
    VegaProcess* proc = calloc(1, sizeof(VegaProcess));
    if (!proc) return NULL;

    proc->pid = vm->next_pid++;
    proc->state = PROC_READY;
    proc->parent_pid = parent_pid;

    // Initialize stack
    proc->sp = 0;
    proc->frame_count = 0;
    proc->ip = 0;

    // Default supervision config
    proc->supervision.strategy = STRATEGY_RESTART;
    proc->supervision.max_restarts = 3;
    proc->supervision.window_ms = 60000;  // 1 minute
    proc->supervision.restart_count = 0;
    proc->supervision.window_start = current_time_ms();

    // Default backoff config
    proc->supervision.backoff = BACKOFF_EXPONENTIAL;
    proc->supervision.base_delay_ms = 1000;    // 1 second base
    proc->supervision.max_delay_ms = 30000;    // 30 second max
    proc->supervision.next_retry_at = 0;

    // Default circuit breaker config
    proc->supervision.circuit_state = CIRCUIT_CLOSED;
    proc->supervision.failure_threshold = 5;
    proc->supervision.failure_count = 0;
    proc->supervision.circuit_opened_at = 0;
    proc->supervision.cooldown_ms = 60000;     // 1 minute cooldown

    proc->is_supervisor = false;
    proc->exit_reason = EXIT_NORMAL;
    proc->exit_message = NULL;
    proc->agent = NULL;
    proc->agent_def_id = 0;
    proc->wait_data = NULL;

    return proc;
}

void process_free(VegaProcess* proc) {
    if (!proc) return;

    // Release stack values
    for (uint32_t i = 0; i < proc->sp; i++) {
        value_release(proc->stack[i]);
    }

    // Break circular reference: agent has a pointer to this process
    // Don't free the agent here - it's owned by the Value on the stack
    // and will be freed via value_release
    if (proc->agent) {
        proc->agent->process = NULL;  // Break the back-reference
        proc->agent = NULL;
    }

    free(proc->exit_message);
    free(proc);
}

uint32_t process_spawn_agent(VegaVM* vm, VegaProcess* parent,
                             uint32_t agent_def_id, SupervisionConfig* config) {
    // Create new process
    VegaProcess* child = process_create(vm, parent ? parent->pid : 0);
    if (!child) return 0;

    // Apply supervision config if provided
    if (config) {
        child->supervision = *config;
    }

    // Mark parent as supervisor
    if (parent) {
        parent->is_supervisor = true;
        process_add_child(parent, child->pid);
    }

    // Store agent definition (actual agent created when process runs)
    child->agent_def_id = agent_def_id;

    // Add to VM's process table
    if (vm->process_count >= MAX_PROCESSES) {
        process_free(child);
        return 0;
    }
    vm->processes[vm->process_count++] = child;

    return child->pid;
}

void process_exit(VegaVM* vm, VegaProcess* proc, ExitReason reason, const char* message) {
    if (!proc || proc->state == PROC_EXITED) return;

    proc->state = PROC_EXITED;
    proc->exit_reason = reason;
    proc->exit_message = message ? strdup(message) : NULL;

    // Notify parent (supervisor)
    if (proc->parent_pid > 0) {
        VegaProcess* parent = NULL;
        for (uint32_t i = 0; i < vm->process_count; i++) {
            if (vm->processes[i]->pid == proc->parent_pid) {
                parent = vm->processes[i];
                break;
            }
        }
        if (parent && parent->state != PROC_EXITED) {
            process_handle_child_exit(vm, parent, proc->pid, reason);
        }
    }

    // Exit all children (propagate failure)
    for (uint32_t i = 0; i < proc->child_count; i++) {
        for (uint32_t j = 0; j < vm->process_count; j++) {
            if (vm->processes[j]->pid == proc->children[i] &&
                vm->processes[j]->state != PROC_EXITED) {
                process_exit(vm, vm->processes[j], EXIT_KILLED, "parent exited");
            }
        }
    }
}

bool process_can_restart(VegaProcess* proc) {
    if (!proc) return false;

    uint64_t now = current_time_ms();

    // Reset window if expired
    if (now - proc->supervision.window_start > proc->supervision.window_ms) {
        proc->supervision.restart_count = 0;
        proc->supervision.window_start = now;
    }

    return proc->supervision.restart_count < proc->supervision.max_restarts;
}

uint32_t process_restart(VegaVM* vm, VegaProcess* proc) {
    if (!proc || !process_can_restart(proc)) return 0;

    // Find parent
    VegaProcess* parent = NULL;
    for (uint32_t i = 0; i < vm->process_count; i++) {
        if (vm->processes[i]->pid == proc->parent_pid) {
            parent = vm->processes[i];
            break;
        }
    }

    // Increment restart count
    proc->supervision.restart_count++;

    // Create new process with same agent
    SupervisionConfig config = proc->supervision;
    uint32_t new_pid = process_spawn_agent(vm, parent, proc->agent_def_id, &config);

    if (new_pid > 0) {
        fprintf(stderr, "[supervisor] Restarting process %u as %u (restart %u/%u)\n",
                proc->pid, new_pid,
                config.restart_count, config.max_restarts);
    }

    return new_pid;
}

// ============================================================================
// Child Management
// ============================================================================

void process_add_child(VegaProcess* parent, uint32_t child_pid) {
    if (!parent || parent->child_count >= MAX_CHILDREN) return;
    parent->children[parent->child_count++] = child_pid;
}

void process_remove_child(VegaProcess* parent, uint32_t child_pid) {
    if (!parent) return;

    for (uint32_t i = 0; i < parent->child_count; i++) {
        if (parent->children[i] == child_pid) {
            // Shift remaining children
            for (uint32_t j = i; j < parent->child_count - 1; j++) {
                parent->children[j] = parent->children[j + 1];
            }
            parent->child_count--;
            return;
        }
    }
}

void process_handle_child_exit(VegaVM* vm, VegaProcess* supervisor,
                               uint32_t child_pid, ExitReason reason) {
    if (!supervisor) return;

    // Find the child process
    VegaProcess* child = NULL;
    for (uint32_t i = 0; i < vm->process_count; i++) {
        if (vm->processes[i]->pid == child_pid) {
            child = vm->processes[i];
            break;
        }
    }
    if (!child) return;

    // Normal exit - just clean up
    if (reason == EXIT_NORMAL) {
        process_remove_child(supervisor, child_pid);
        return;
    }

    // Handle based on strategy
    switch (child->supervision.strategy) {
        case STRATEGY_RESTART:
            if (process_can_restart(child)) {
                process_restart(vm, child);
            } else {
                fprintf(stderr, "[supervisor] Process %u exceeded max restarts, stopping\n",
                        child_pid);
                process_remove_child(supervisor, child_pid);
            }
            break;

        case STRATEGY_STOP:
            fprintf(stderr, "[supervisor] Process %u stopped (strategy=stop)\n", child_pid);
            process_remove_child(supervisor, child_pid);
            break;

        case STRATEGY_ESCALATE:
            fprintf(stderr, "[supervisor] Process %u failed, escalating to parent\n", child_pid);
            process_remove_child(supervisor, child_pid);
            process_exit(vm, supervisor, EXIT_ERROR, "child escalated failure");
            break;

        case STRATEGY_RESTART_ALL:
            fprintf(stderr, "[supervisor] Process %u failed, restarting all children\n",
                    child_pid);
            // Restart all children
            for (uint32_t i = 0; i < supervisor->child_count; i++) {
                for (uint32_t j = 0; j < vm->process_count; j++) {
                    if (vm->processes[j]->pid == supervisor->children[i]) {
                        process_restart(vm, vm->processes[j]);
                    }
                }
            }
            break;
    }
}

// ============================================================================
// Backoff and Circuit Breaker
// ============================================================================

int32_t process_schedule_retry(VegaProcess* proc) {
    if (!proc) return -1;

    uint64_t now = current_time_ms();
    SupervisionConfig* cfg = &proc->supervision;

    // Check if already past retry time
    if (cfg->next_retry_at > 0 && now < cfg->next_retry_at) {
        return (int32_t)(cfg->next_retry_at - now);
    }

    // Check if can restart at all
    if (!process_can_restart(proc)) {
        return -1;
    }

    // Calculate delay based on backoff strategy
    uint32_t delay = 0;
    uint32_t attempt = cfg->restart_count;

    switch (cfg->backoff) {
        case BACKOFF_NONE:
            delay = 0;
            break;

        case BACKOFF_LINEAR:
            delay = cfg->base_delay_ms * (attempt + 1);
            break;

        case BACKOFF_EXPONENTIAL:
            // 2^attempt, but avoid overflow
            if (attempt < 16) {
                delay = cfg->base_delay_ms * (1u << attempt);
            } else {
                delay = cfg->max_delay_ms;
            }
            break;
    }

    // Cap at max delay
    if (delay > cfg->max_delay_ms) {
        delay = cfg->max_delay_ms;
    }

    // Schedule next retry
    cfg->next_retry_at = now + delay;

    return delay == 0 ? 0 : (int32_t)delay;
}

bool process_circuit_allows(VegaProcess* proc) {
    if (!proc) return false;

    uint64_t now = current_time_ms();
    SupervisionConfig* cfg = &proc->supervision;

    switch (cfg->circuit_state) {
        case CIRCUIT_CLOSED:
            return true;

        case CIRCUIT_OPEN:
            // Check if cooldown has expired
            if (now - cfg->circuit_opened_at >= cfg->cooldown_ms) {
                cfg->circuit_state = CIRCUIT_HALF_OPEN;
                fprintf(stderr, "[circuit-breaker] Process %u: circuit half-open, allowing test request\n",
                        proc->pid);
                return true;
            }
            return false;

        case CIRCUIT_HALF_OPEN:
            // Allow one test request in half-open state
            return true;
    }

    return false;
}

void process_record_success(VegaProcess* proc) {
    if (!proc) return;

    SupervisionConfig* cfg = &proc->supervision;

    // Reset failure count
    cfg->failure_count = 0;

    // Close circuit if it was half-open
    if (cfg->circuit_state == CIRCUIT_HALF_OPEN) {
        cfg->circuit_state = CIRCUIT_CLOSED;
        fprintf(stderr, "[circuit-breaker] Process %u: circuit closed after successful request\n",
                proc->pid);
    }
}

void process_record_failure(VegaProcess* proc) {
    if (!proc) return;

    SupervisionConfig* cfg = &proc->supervision;

    cfg->failure_count++;

    // Check if should open circuit
    if (cfg->circuit_state == CIRCUIT_CLOSED &&
        cfg->failure_count >= cfg->failure_threshold) {
        cfg->circuit_state = CIRCUIT_OPEN;
        cfg->circuit_opened_at = current_time_ms();
        fprintf(stderr, "[circuit-breaker] Process %u: circuit opened after %u failures\n",
                proc->pid, cfg->failure_count);
    }

    // If half-open and failed, reopen
    if (cfg->circuit_state == CIRCUIT_HALF_OPEN) {
        cfg->circuit_state = CIRCUIT_OPEN;
        cfg->circuit_opened_at = current_time_ms();
        fprintf(stderr, "[circuit-breaker] Process %u: circuit reopened after test failure\n",
                proc->pid);
    }
}

// ============================================================================
// Stack Operations
// ============================================================================

void process_push(VegaProcess* proc, Value v) {
    if (!proc || proc->sp >= PROCESS_STACK_SIZE) return;
    value_retain(v);
    proc->stack[proc->sp++] = v;
}

Value process_pop(VegaProcess* proc) {
    if (!proc || proc->sp == 0) return value_null();
    return proc->stack[--proc->sp];
}

Value process_peek(VegaProcess* proc, uint32_t distance) {
    if (!proc || distance >= proc->sp) return value_null();
    return proc->stack[proc->sp - 1 - distance];
}

// ============================================================================
// Debug
// ============================================================================

static const char* state_name(ProcessState state) {
    switch (state) {
        case PROC_READY:   return "ready";
        case PROC_RUNNING: return "running";
        case PROC_WAITING: return "waiting";
        case PROC_EXITED:  return "exited";
        default:           return "unknown";
    }
}

static const char* reason_name(ExitReason reason) {
    switch (reason) {
        case EXIT_NORMAL: return "normal";
        case EXIT_ERROR:  return "error";
        case EXIT_KILLED: return "killed";
        default:          return "unknown";
    }
}

void process_print(VegaProcess* proc) {
    if (!proc) return;

    printf("Process %u:\n", proc->pid);
    printf("  state: %s\n", state_name(proc->state));
    printf("  parent: %u\n", proc->parent_pid);
    printf("  children: %u\n", proc->child_count);
    printf("  stack depth: %u\n", proc->sp);
    printf("  frame count: %u\n", proc->frame_count);

    if (proc->state == PROC_EXITED) {
        printf("  exit reason: %s\n", reason_name(proc->exit_reason));
        if (proc->exit_message) {
            printf("  exit message: %s\n", proc->exit_message);
        }
    }

    if (proc->agent) {
        printf("  agent: %s\n", agent_get_name(proc->agent));
    }
}
