#include "scheduler.h"
#include "vm.h"
#include <stdlib.h>
#include <stdio.h>

// ============================================================================
// Queue Operations
// ============================================================================

void queue_init(ProcessQueue* q, uint32_t capacity) {
    q->items = calloc(capacity, sizeof(uint32_t));
    q->capacity = capacity;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
}

void queue_free(ProcessQueue* q) {
    free(q->items);
    q->items = NULL;
    q->capacity = 0;
    q->head = 0;
    q->tail = 0;
    q->count = 0;
}

bool queue_push(ProcessQueue* q, uint32_t pid) {
    if (q->count >= q->capacity) {
        // Grow queue
        uint32_t new_cap = q->capacity * 2;
        uint32_t* new_items = calloc(new_cap, sizeof(uint32_t));
        if (!new_items) return false;

        // Copy items in order
        for (uint32_t i = 0; i < q->count; i++) {
            new_items[i] = q->items[(q->head + i) % q->capacity];
        }

        free(q->items);
        q->items = new_items;
        q->capacity = new_cap;
        q->head = 0;
        q->tail = q->count;
    }

    q->items[q->tail] = pid;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    return true;
}

bool queue_pop(ProcessQueue* q, uint32_t* pid) {
    if (q->count == 0) return false;

    *pid = q->items[q->head];
    q->head = (q->head + 1) % q->capacity;
    q->count--;
    return true;
}

bool queue_is_empty(ProcessQueue* q) {
    return q->count == 0;
}

// ============================================================================
// Scheduler Lifecycle
// ============================================================================

void scheduler_init(Scheduler* sched, VegaProcess** processes, uint32_t* count) {
    sched->processes = processes;
    sched->process_count = count;
    sched->current = NULL;
    sched->context_switches = 0;
    sched->processes_spawned = 0;
    sched->processes_exited = 0;

    queue_init(&sched->ready_queue, 64);
}

void scheduler_cleanup(Scheduler* sched) {
    queue_free(&sched->ready_queue);
    sched->processes = NULL;
    sched->process_count = NULL;
    sched->current = NULL;
}

// ============================================================================
// Process Lookup
// ============================================================================

static VegaProcess* find_process(Scheduler* sched, uint32_t pid) {
    for (uint32_t i = 0; i < *sched->process_count; i++) {
        if (sched->processes[i]->pid == pid) {
            return sched->processes[i];
        }
    }
    return NULL;
}

// ============================================================================
// Scheduler Operations
// ============================================================================

void scheduler_enqueue(Scheduler* sched, uint32_t pid) {
    VegaProcess* proc = find_process(sched, pid);
    if (!proc) return;

    // Only enqueue if ready
    if (proc->state == PROC_READY) {
        queue_push(&sched->ready_queue, pid);
    }
}

VegaProcess* scheduler_next(Scheduler* sched) {
    uint32_t pid;

    // Find next ready process
    while (queue_pop(&sched->ready_queue, &pid)) {
        VegaProcess* proc = find_process(sched, pid);
        if (proc && proc->state == PROC_READY) {
            proc->state = PROC_RUNNING;
            sched->current = proc;
            sched->context_switches++;
            return proc;
        }
        // Skip exited/invalid processes
    }

    sched->current = NULL;
    return NULL;
}

void scheduler_yield(Scheduler* sched) {
    if (!sched->current) return;

    VegaProcess* proc = sched->current;
    if (proc->state == PROC_RUNNING) {
        proc->state = PROC_READY;
        queue_push(&sched->ready_queue, proc->pid);
    }
    sched->current = NULL;
}

void scheduler_block(Scheduler* sched) {
    if (!sched->current) return;

    VegaProcess* proc = sched->current;
    if (proc->state == PROC_RUNNING) {
        proc->state = PROC_WAITING;
    }
    sched->current = NULL;
}

void scheduler_unblock(Scheduler* sched, uint32_t pid) {
    VegaProcess* proc = find_process(sched, pid);
    if (!proc) return;

    if (proc->state == PROC_WAITING) {
        proc->state = PROC_READY;
        queue_push(&sched->ready_queue, pid);
    }
}

bool scheduler_has_runnable(Scheduler* sched) {
    // Check if any process is ready or running
    for (uint32_t i = 0; i < *sched->process_count; i++) {
        ProcessState state = sched->processes[i]->state;
        if (state == PROC_READY || state == PROC_RUNNING || state == PROC_WAITING) {
            return true;
        }
    }
    return false;
}

VegaProcess* scheduler_current(Scheduler* sched) {
    return sched->current;
}

// ============================================================================
// Main Run Loop
// ============================================================================

void scheduler_run(VegaVM* vm) {
    Scheduler* sched = &vm->scheduler;

    while (scheduler_has_runnable(sched)) {
        // Get next process to run
        VegaProcess* proc = scheduler_next(sched);
        if (!proc) {
            // No ready processes, but some are waiting
            // In real implementation, would poll I/O here
            break;
        }

        // Execute process until it yields or exits
        // This will be implemented in vm.c
        vm_execute_process(vm, proc);

        // Process may have yielded, blocked, or exited
        // State updates happen in the yield/block/exit functions
    }
}

// ============================================================================
// Debug
// ============================================================================

void scheduler_print(Scheduler* sched) {
    printf("Scheduler state:\n");
    printf("  context switches: %llu\n", (unsigned long long)sched->context_switches);
    printf("  processes spawned: %llu\n", (unsigned long long)sched->processes_spawned);
    printf("  processes exited: %llu\n", (unsigned long long)sched->processes_exited);
    printf("  ready queue size: %u\n", sched->ready_queue.count);
    printf("  current process: %s\n",
           sched->current ? "yes" : "none");

    if (sched->current) {
        printf("    pid: %u\n", sched->current->pid);
    }

    printf("  all processes:\n");
    for (uint32_t i = 0; i < *sched->process_count; i++) {
        VegaProcess* p = sched->processes[i];
        const char* state_str;
        switch (p->state) {
            case PROC_READY:   state_str = "ready"; break;
            case PROC_RUNNING: state_str = "running"; break;
            case PROC_WAITING: state_str = "waiting"; break;
            case PROC_EXITED:  state_str = "exited"; break;
            default:           state_str = "unknown"; break;
        }
        printf("    [%u] pid=%u state=%s\n", i, p->pid, state_str);
    }
}
