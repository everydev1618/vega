# Vega Language Specification

## Overview

Vega is a compiled language for fault-tolerant AI agent orchestration. It compiles to bytecode and runs on the Vega VM, which provides supervision, automatic parallelization, and resource management as first-class runtime features.

**Design philosophy:** The runtime does the hard work. Users write simple, sequential-looking code. The compiler and VM handle parallelism, fault tolerance, retries, and resource management automatically.

---

## Core Differentiators

What makes Vega worth using instead of Python + Anthropic SDK:

1. **Supervision trees** - Agents can fail. The runtime restarts them automatically with configurable strategies.
2. **Automatic parallelization** - Write sequential code, get parallel execution. The compiler analyzes dataflow.
3. **Budgets as primitives** - Cost limits enforced at the language level, not as an afterthought.
4. **Built-in backpressure** - Rate limits, retries, and circuit breakers handled by the VM.
5. **Observability by default** - Every agent call is traced. Costs, tokens, latency tracked automatically.

---

## Language Constructs

### Agents

```vega
agent Coder {
    model "claude-sonnet-4-20250514"
    system "You write clean, efficient code."
    temperature 0.3

    // Budget per invocation (optional)
    budget $0.50

    // Retry policy (optional, has sensible defaults)
    retry {
        max_attempts 3
        backoff exponential(100ms, 2.0, 5s)  // base, multiplier, max
        on [rate_limit, overloaded, timeout]  // which errors to retry
    }

    tool read_file(path: str) -> str {
        return file::read(path);
    }
}
```

### Spawning and Supervision

```vega
// Basic spawn - runtime supervises with default strategy
let coder = spawn Coder;

// Spawn with explicit supervision
let coder = spawn Coder supervised by {
    strategy restart         // restart | stop | escalate
    max_restarts 3          // within time window
    window 60s
};

// Spawn async (returns future)
let future = spawn async Coder;
let result = await future;
```

### Message Passing

```vega
// Synchronous send - blocks until response
let code = coder <- "Write a sorting function";

// Send with timeout
let code = coder <- "Write code" timeout 30s;

// Send with budget override
let code = coder <- "Write code" budget $1.00;

// Streaming response
for chunk in coder <~ "Write a long story" {
    print(chunk);
}
```

### Automatic Parallelization

The compiler performs dataflow analysis. Independent operations run in parallel automatically:

```vega
// These three calls have no data dependencies
// Compiler parallelizes automatically
let review1 = reviewer <- code1;
let review2 = reviewer <- code2;
let review3 = reviewer <- code3;

// This depends on all three - waits for all to complete
let summary = summarizer <- review1 + review2 + review3;
```

Explicit parallel construct for clarity:

```vega
parallel {
    let a = agent1 <- msg1;
    let b = agent2 <- msg2;
}
// Both a and b available here
```

### Budgets

```vega
// Session-level budget
session budget $10.00 {
    let coder = spawn Coder;
    let reviewer = spawn Reviewer;

    // All agent calls within this block share the $10 budget
    let code = coder <- task;
    let review = reviewer <- code;
}
// Throws BudgetExceeded if limit hit

// Query remaining budget
let remaining = budget::remaining();
```

### Error Handling

```vega
// Supervision handles most errors automatically, but you can catch:
try {
    let result = coder <- task;
} catch BudgetExceeded {
    print("Out of budget");
} catch AgentFailed(reason) {
    print("Agent failed after retries: " + reason);
}

// Circuit breaker (automatic, but configurable)
agent FlakeyService {
    circuit_breaker {
        threshold 5          // failures before opening
        reset_after 30s      // time before half-open
    }
}
```

---

## VM Architecture

### Process Model

The Vega VM uses a lightweight process model inspired by Erlang:

- Each spawned agent runs in its own **process** (not OS thread)
- Processes are cheap (thousands can run concurrently)
- Processes don't share memory - communicate via message passing
- Process failure is isolated - doesn't crash the VM

### Supervision Trees

```
           [Root Supervisor]
                  |
     +------------+------------+
     |            |            |
 [Coder]     [Reviewer]   [Sub-Supervisor]
                               |
                        +------+------+
                        |             |
                    [Worker1]    [Worker2]
```

Supervision strategies:
- `restart` - Restart the failed process
- `stop` - Stop the process, notify parent
- `escalate` - Let the supervisor's supervisor handle it
- `restart_all` - Restart all children (for interdependent processes)

### Scheduler

- M:N threading (M processes on N OS threads)
- Work-stealing scheduler for load balancing
- Preemption at await points (agent calls, explicit yields)
- Priority levels for different agent types

### Resource Management

The VM tracks and enforces:
- Token usage (input/output)
- API costs (calculated from token counts)
- Request rate (per-model, per-agent)
- Concurrent request limits

---

## Bytecode Extensions

New opcodes for runtime features:

```
// Supervision (0x90 - 0x9F)
OP_SPAWN_SUPERVISED  = 0x90  // Spawn with supervision config
OP_LINK              = 0x91  // Link two processes (bidirectional failure notification)
OP_MONITOR           = 0x92  // Monitor a process (unidirectional)
OP_EXIT              = 0x93  // Exit current process with reason

// Streaming (0xA0 - 0xAF)
OP_SEND_STREAM       = 0xA0  // Send message, get stream iterator
OP_STREAM_NEXT       = 0xA1  // Get next chunk from stream
OP_STREAM_DONE       = 0xA2  // Check if stream exhausted

// Budget (0xB0 - 0xBF)
OP_BUDGET_ENTER      = 0xB0  // Enter budget scope
OP_BUDGET_EXIT       = 0xB1  // Exit budget scope
OP_BUDGET_CHECK      = 0xB2  // Check remaining budget
OP_BUDGET_RESERVE    = 0xB3  // Reserve amount before call

// Parallel (0xC0 - 0xCF)
OP_PAR_BEGIN         = 0xC0  // Begin parallel block
OP_PAR_SPAWN         = 0xC1  // Spawn parallel task
OP_PAR_JOIN          = 0xC2  // Wait for all parallel tasks
```

---

## Standard Library

### Core Modules

```vega
// Budget management
budget::remaining() -> float
budget::used() -> float
budget::limit() -> float

// Process management
process::self() -> ProcessId
process::exit(reason: str)
process::sleep(duration: duration)

// Tracing
trace::span(name: str) -> Span
trace::event(msg: str)
trace::set_attribute(key: str, value: any)

// File I/O (sandboxed)
file::read(path: str) -> str
file::write(path: str, content: str)
file::exists(path: str) -> bool

// HTTP (sandboxed)
http::get(url: str) -> Response
http::post(url: str, body: str) -> Response
```

---

## Observability

Every agent invocation automatically records:

```json
{
  "trace_id": "abc123",
  "span_id": "def456",
  "agent": "Coder",
  "model": "claude-sonnet-4-20250514",
  "input_tokens": 150,
  "output_tokens": 892,
  "cost_usd": 0.0043,
  "latency_ms": 2341,
  "status": "ok",
  "retries": 0,
  "parent_span": "xyz789"
}
```

Export formats:
- OTLP (OpenTelemetry)
- JSON lines
- SQLite (for local debugging)

---

## Capability-Based Tool Security

Tools have explicit capabilities:

```vega
agent Coder {
    tool read_file(path: str) -> str
        capabilities [fs::read]
        allowed_paths ["/project/**"]
    {
        return file::read(path);
    }

    tool run_tests() -> str
        capabilities [process::exec]
        allowed_commands ["npm test", "pytest"]
    {
        return exec("npm test");
    }
}
```

The VM enforces these at runtime - a tool cannot exceed its declared capabilities.
