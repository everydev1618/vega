# Supervision System Design

## Overview

Vega uses an Erlang-inspired process model where each agent runs in a lightweight process. Processes are supervised and can be automatically restarted on failure.

## Core Concepts

### Process

A process is a lightweight execution context:
- Has its own stack and local variables
- Has a mailbox for receiving messages
- Has a supervisor (parent process)
- Can spawn child processes
- Can fail without crashing the VM

```c
typedef struct VegaProcess {
    uint32_t pid;           // Process ID
    ProcessState state;     // running, waiting, exited

    // Execution state
    uint32_t ip;            // Instruction pointer
    Value* stack;           // Process-local stack
    uint32_t sp;
    CallFrame* frames;      // Call stack
    uint32_t frame_count;

    // Relationships
    uint32_t parent_pid;    // Supervisor
    uint32_t* children;     // Spawned processes
    uint32_t child_count;

    // Failure handling
    ExitReason exit_reason; // normal, error, killed
    char* exit_message;

    // Agent (if this process runs an agent)
    VegaAgent* agent;
} VegaProcess;
```

### Scheduler

The scheduler manages process execution:
- Maintains a run queue of ready processes
- Yields at await points (agent calls, sleep, yield)
- Handles process creation and termination

```c
typedef struct Scheduler {
    VegaProcess** processes;    // Process table
    uint32_t process_count;
    uint32_t next_pid;

    // Run queues
    Queue* ready_queue;         // Processes ready to run
    Queue* waiting_queue;       // Processes waiting for I/O

    VegaProcess* current;       // Currently running process
} Scheduler;
```

### Supervision Tree

Supervisors monitor their children and handle failures:

```
           [Root (main)]
                 |
    +------------+------------+
    |                         |
 [Coder]                 [Reviewer]
```

### Restart Strategies

- **restart**: Restart the failed child
- **stop**: Let the child stay dead, notify parent
- **escalate**: Crash self, let parent handle it
- **restart_all**: Restart all children (for interdependent processes)

## Implementation Plan

### Phase 2.1: Process Model (Current)

1. Create `VegaProcess` struct
2. Create process table in VM
3. Make agents run in processes
4. Add process spawn/exit

### Phase 2.2: Scheduler

1. Run queue management
2. Yield at agent calls
3. Resume after API response
4. Context switching

### Phase 2.3: Supervision

1. Parent-child relationships
2. Exit propagation
3. Restart strategies
4. Max restarts tracking

### Phase 2.4: Language Support

1. `spawn supervised by { }` syntax
2. Parser updates
3. New opcodes: SPAWN_SUPERVISED, LINK, EXIT

## New Files

```
src/vm/process.h   - Process struct and API
src/vm/process.c   - Process implementation
src/vm/scheduler.h - Scheduler struct and API
src/vm/scheduler.c - Scheduler implementation
```

## Modified Files

```
src/vm/vm.h        - Add scheduler to VM state
src/vm/vm.c        - Integrate scheduler, yield at await points
src/vm/agent.c     - Agents run in processes
src/compiler/lexer.c  - New keywords: supervised, strategy
src/compiler/parser.c - Parse supervision syntax
src/compiler/codegen.c - Emit supervision opcodes
```

## Bytecode Changes

```c
// New opcodes
OP_SPAWN_PROCESS     = 0x90  // Create new process
OP_EXIT_PROCESS      = 0x91  // Exit current process
OP_YIELD             = 0x92  // Yield to scheduler
OP_SPAWN_SUPERVISED  = 0x93  // Spawn with supervision config
OP_LINK              = 0x94  // Link two processes
OP_MONITOR           = 0x95  // Monitor a process
```

## API Changes

When an agent makes an API call:
1. Current process yields
2. Scheduler picks another ready process
3. When API response arrives, process becomes ready
4. Eventually scheduler resumes it

## Failure Handling

When a process fails:
1. Exit reason captured (error message)
2. Children notified (if any)
3. Parent (supervisor) notified
4. Supervisor applies restart strategy
5. If restart: new process spawned with same agent
6. If stop: parent receives exit notification
7. If escalate: parent also exits

## Example

```claw
fn main() {
    // Spawn with supervision
    let coder = spawn Coder supervised by {
        strategy restart
        max_restarts 3
        window 60s
    };

    // If this fails, coder gets restarted (up to 3 times in 60s)
    let code = coder <- "Write a function";
}
```
