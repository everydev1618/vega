#ifndef VEGA_SCHEDULER_H
#define VEGA_SCHEDULER_H

#include "process.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Vega Scheduler
 *
 * Cooperative scheduler for lightweight processes.
 * Processes yield at await points (agent calls, sleep, explicit yield).
 */

// ============================================================================
// Types
// ============================================================================

// Simple queue for run queue
typedef struct {
    uint32_t* items;
    uint32_t capacity;
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} ProcessQueue;

struct VegaVM;

typedef struct Scheduler {
    // Process table (owned by VM, referenced here)
    VegaProcess** processes;
    uint32_t* process_count;

    // Run queue (PIDs of ready processes)
    ProcessQueue ready_queue;

    // Current process
    VegaProcess* current;

    // Stats
    uint64_t context_switches;
    uint64_t processes_spawned;
    uint64_t processes_exited;

} Scheduler;

// ============================================================================
// Scheduler API
// ============================================================================

// Initialize scheduler
void scheduler_init(Scheduler* sched, VegaProcess** processes, uint32_t* count);

// Cleanup scheduler
void scheduler_cleanup(Scheduler* sched);

// Add process to ready queue
void scheduler_enqueue(Scheduler* sched, uint32_t pid);

// Get next process to run (returns NULL if none ready)
VegaProcess* scheduler_next(Scheduler* sched);

// Yield current process (put back in ready queue)
void scheduler_yield(Scheduler* sched);

// Block current process (waiting for I/O)
void scheduler_block(Scheduler* sched);

// Unblock a process (I/O completed)
void scheduler_unblock(Scheduler* sched, uint32_t pid);

// Run scheduler loop until all processes exit
void scheduler_run(struct VegaVM* vm);

// Check if any processes are still running
bool scheduler_has_runnable(Scheduler* sched);

// Get current process
VegaProcess* scheduler_current(Scheduler* sched);

// Debug: print scheduler state
void scheduler_print(Scheduler* sched);

// ============================================================================
// Queue Operations
// ============================================================================

void queue_init(ProcessQueue* q, uint32_t capacity);
void queue_free(ProcessQueue* q);
bool queue_push(ProcessQueue* q, uint32_t pid);
bool queue_pop(ProcessQueue* q, uint32_t* pid);
bool queue_is_empty(ProcessQueue* q);

#endif // VEGA_SCHEDULER_H
