#ifndef VEGA_PROCESS_H
#define VEGA_PROCESS_H

#include "value.h"
#include "../common/bytecode.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Vega Process Model
 *
 * Lightweight processes for concurrent agent execution.
 * Inspired by Erlang's process model.
 */

// ============================================================================
// Constants
// ============================================================================

#define PROCESS_STACK_SIZE  256
#define PROCESS_FRAMES_MAX  32
#define MAX_PROCESSES       1024
#define MAX_CHILDREN        64

// ============================================================================
// Types
// ============================================================================

typedef enum {
    PROC_READY,         // Ready to run
    PROC_RUNNING,       // Currently executing
    PROC_WAITING,       // Waiting for I/O (agent call)
    PROC_EXITED,        // Process has terminated
} ProcessState;

typedef enum {
    EXIT_NORMAL,        // Process completed successfully
    EXIT_ERROR,         // Process crashed with error
    EXIT_KILLED,        // Process was killed externally
} ExitReason;

typedef enum {
    STRATEGY_RESTART,       // Restart the failed child
    STRATEGY_STOP,          // Let child stay dead
    STRATEGY_ESCALATE,      // Crash self, let parent handle
    STRATEGY_RESTART_ALL,   // Restart all children
} RestartStrategy;

// Call frame for process-local call stack
typedef struct {
    uint32_t function_id;
    uint32_t ip;            // Return address
    uint32_t bp;            // Base pointer
} ProcessFrame;

// Supervision configuration
typedef struct {
    RestartStrategy strategy;
    uint32_t max_restarts;      // Max restarts in window
    uint32_t window_ms;         // Time window in milliseconds
    uint32_t restart_count;     // Current restart count
    uint64_t window_start;      // When window started
} SupervisionConfig;

// Forward declaration
struct VegaAgent;
struct VegaVM;

// Process structure
typedef struct VegaProcess {
    uint32_t pid;               // Process ID
    ProcessState state;

    // Execution state
    uint32_t ip;                // Instruction pointer
    Value stack[PROCESS_STACK_SIZE];
    uint32_t sp;                // Stack pointer
    ProcessFrame frames[PROCESS_FRAMES_MAX];
    uint32_t frame_count;

    // Relationships
    uint32_t parent_pid;        // 0 = no parent (root process)
    uint32_t children[MAX_CHILDREN];
    uint32_t child_count;

    // Supervision
    SupervisionConfig supervision;
    bool is_supervisor;         // Does this process supervise others?

    // Exit state
    ExitReason exit_reason;
    char* exit_message;

    // Agent (if this process runs an agent)
    struct VegaAgent* agent;
    uint32_t agent_def_id;      // Which agent definition to use

    // Waiting state (for async operations)
    void* wait_data;            // Data being waited on (e.g., HTTP response)

} VegaProcess;

// ============================================================================
// Process API
// ============================================================================

// Create a new process (called from scheduler)
VegaProcess* process_create(struct VegaVM* vm, uint32_t parent_pid);

// Free a process
void process_free(VegaProcess* proc);

// Spawn a child process for an agent
uint32_t process_spawn_agent(struct VegaVM* vm, VegaProcess* parent,
                             uint32_t agent_def_id, SupervisionConfig* config);

// Exit a process with reason
void process_exit(struct VegaVM* vm, VegaProcess* proc,
                  ExitReason reason, const char* message);

// Check if process can be restarted
bool process_can_restart(VegaProcess* proc);

// Restart a process (creates new process with same agent)
uint32_t process_restart(struct VegaVM* vm, VegaProcess* proc);

// Add child to parent's child list
void process_add_child(VegaProcess* parent, uint32_t child_pid);

// Remove child from parent's child list
void process_remove_child(VegaProcess* parent, uint32_t child_pid);

// Handle child exit (called by supervisor)
void process_handle_child_exit(struct VegaVM* vm, VegaProcess* supervisor,
                               uint32_t child_pid, ExitReason reason);

// Push value onto process stack
void process_push(VegaProcess* proc, Value v);

// Pop value from process stack
Value process_pop(VegaProcess* proc);

// Peek at stack
Value process_peek(VegaProcess* proc, uint32_t distance);

// Debug: print process state
void process_print(VegaProcess* proc);

#endif // VEGA_PROCESS_H
