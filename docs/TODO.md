# Vega Development Roadmap

## Prioritization Rationale

Features are prioritized by:
1. **Differentiation** - Does this make Vega worth using over Python + SDK?
2. **Foundation** - Does other stuff depend on this?
3. **Pain point** - Does this solve a real problem people have today?

---

## Phase 0: Foundation ✓ COMPLETE

What exists today:
- [x] Lexer
- [x] Parser
- [x] AST definitions
- [x] Bytecode specification
- [x] Code generation (compiler backend)
- [x] VM implementation
- [x] Basic Anthropic API integration

**Status:** Complete. Compiler and VM fully functional.

---

## Phase 1: Core Runtime ✓ COMPLETE

Priority: **CRITICAL** - Without this, nothing else matters.

### 1.1 Basic VM
- [x] Stack machine implementation
- [x] Bytecode loader
- [x] Constant pool
- [x] Local/global variables
- [x] Control flow (jumps, calls, returns)
- [x] Basic type system (int, str, bool, null)

### 1.2 Agent Execution
- [x] Agent registry (load agent definitions from bytecode)
- [x] Anthropic API client (libcurl)
- [x] Basic `spawn` - create agent handle
- [x] Basic `<-` - send message, get response
- [x] Tool execution (call Vega function when Claude uses tool)

### 1.3 Compiler Backend
- [x] AST to bytecode for expressions
- [x] AST to bytecode for statements (including for/break/continue)
- [x] AST to bytecode for functions
- [x] AST to bytecode for agents (including tools)
- [x] Bytecode serialization (.vgb file writing)

**Milestone:** ✓ Can compile and run examples end-to-end.

---

## Phase 2: Supervision & Fault Tolerance ✓ MOSTLY COMPLETE

Priority: **HIGH** - This is THE differentiator.

### 2.1 Process Model ✓
- [x] Lightweight process abstraction (VegaProcess struct)
- [x] Process table (spawn, lookup, kill)
- [x] Process-local stack (isolated stack per process)
- [ ] Message queues between processes

### 2.2 Scheduler ✓
- [x] Cooperative scheduling infrastructure
- [x] Run queue management (ProcessQueue)
- [x] Context switching (vm_execute_process)
- [x] OP_YIELD opcode (basic implementation)

### 2.3 Supervision Trees ✓
- [x] Supervisor process type (is_supervisor flag)
- [x] Parent-child relationships
- [x] Exit reasons (normal, error, killed)
- [x] Restart strategies: `restart`, `stop`, `escalate`, `restart_all`
- [x] Max restarts / time window tracking
- [x] `spawn supervised by { ... }` syntax (lexer, parser, codegen, VM)
- [ ] Supervision tree visualization (debug tool)

### 2.4 Error Propagation
- [x] Exit reasons implementation
- [x] Propagate failures up supervision tree
- [ ] `try/catch` for explicit error handling
- [ ] Unhandled error logging

**Milestone:** Agent that crashes gets restarted automatically. Supervision tree works.

**Status:** Core supervision infrastructure complete. Language syntax implemented.

**Files Added/Modified:**
- `src/vm/process.h/c` - Process abstraction
- `src/vm/scheduler.h/c` - Cooperative scheduler
- `src/vm/agent.h/c` - Supervised spawn support
- `src/vm/vm.h/c` - Process table, scheduler integration
- `src/compiler/lexer.h/c` - Supervision keywords
- `src/compiler/parser.c` - Supervision syntax parsing
- `src/compiler/ast.h/c` - AST nodes for supervision
- `src/compiler/codegen.c` - Emit OP_SPAWN_SUPERVISED
- `src/common/bytecode.h` - Process/supervision opcodes

---

## Phase 3: Automatic Parallelization

Priority: **HIGH** - Major productivity win.

### 3.1 Dataflow Analysis (Compiler)
- [ ] Build dependency graph from AST
- [ ] Identify independent agent calls
- [ ] Identify join points (where parallel results merge)
- [ ] Dead code detection (nice side effect)

### 3.2 Parallel Codegen
- [ ] Emit `OP_PAR_BEGIN`, `OP_PAR_SPAWN`, `OP_PAR_JOIN`
- [ ] Parallel block scoping (variables available after join)

### 3.3 Parallel Runtime
- [ ] Fork-join execution in VM
- [ ] Result collection from parallel branches
- [ ] Error handling in parallel blocks (fail-fast vs collect-all)

### 3.4 Explicit Parallel Construct
- [ ] `parallel { }` block syntax
- [ ] Parser support
- [ ] Same codegen as auto-parallel

**Milestone:** Write sequential code, see parallel API calls in traces.

---

## Phase 4: Budgets & Cost Control

Priority: **HIGH** - Real pain point, easy win.

### 4.1 Cost Tracking
- [ ] Token counting (input/output per call)
- [ ] Cost calculation per model (price table)
- [ ] Accumulator per session/scope

### 4.2 Budget Enforcement
- [ ] `budget` clause on agents
- [ ] `session budget $X { }` blocks
- [ ] `OP_BUDGET_ENTER`, `OP_BUDGET_EXIT`, `OP_BUDGET_CHECK`
- [ ] Pre-call estimation (reserve before sending)
- [ ] `BudgetExceeded` exception

### 4.3 Budget API
- [ ] `budget::remaining()`, `budget::used()`, `budget::limit()`
- [ ] Budget inheritance (nested scopes)

**Milestone:** Set a $5 budget, get `BudgetExceeded` when it runs out.

---

## Phase 5: Retry & Backpressure

Priority: **MEDIUM** - Important for production, but can fake it initially.

### 5.1 Retry Logic
- [ ] Configurable retry policy per agent
- [ ] Exponential backoff implementation
- [ ] Retryable error classification (rate_limit, overloaded, timeout)
- [ ] Retry budget (don't retry forever)

### 5.2 Rate Limiting
- [ ] Per-model rate limit tracking
- [ ] Token bucket / leaky bucket
- [ ] Request queuing when at limit
- [ ] Backpressure signal to callers

### 5.3 Circuit Breaker
- [ ] Failure threshold tracking
- [ ] Open/half-open/closed states
- [ ] Automatic recovery attempts
- [ ] `circuit_breaker { }` config syntax

**Milestone:** Hit rate limit, see automatic backoff and retry in logs.

---

## Phase 6: Streaming

Priority: **MEDIUM** - Important for UX, changes semantics.

### 6.1 Streaming API Integration
- [ ] SSE parsing from Anthropic API
- [ ] Chunk accumulation
- [ ] Stream cancellation

### 6.2 Language Support
- [ ] `<~` streaming send operator
- [ ] `for chunk in stream { }` iteration
- [ ] Stream as first-class value

### 6.3 Runtime Support
- [ ] `OP_SEND_STREAM`, `OP_STREAM_NEXT`, `OP_STREAM_DONE`
- [ ] Backpressure for slow consumers

**Milestone:** Stream a long response, see chunks arrive incrementally.

---

## Phase 7: Observability

Priority: **MEDIUM** - Needed for debugging, can start simple.

### 7.1 Basic Tracing
- [ ] Trace ID generation
- [ ] Span creation per agent call
- [ ] Automatic attribute recording (tokens, cost, latency)
- [ ] Parent-child span relationships

### 7.2 Export
- [ ] JSON lines to file
- [ ] SQLite for local queries
- [ ] OTLP export (OpenTelemetry)

### 7.3 Trace API
- [ ] `trace::span()`, `trace::event()`, `trace::set_attribute()`
- [ ] Manual span creation

### 7.4 Debug Tools
- [ ] CLI: `claw trace <file.vgb>` - run with tracing
- [ ] CLI: `claw trace-view <trace.db>` - view traces

**Milestone:** Run a program, query SQLite to see all agent calls with costs.

---

## Phase 8: Context & Memory

Priority: **LOW** - Complex, can defer.

### 8.1 Conversation History
- [ ] Message history per agent handle
- [ ] Automatic history inclusion in API calls
- [ ] History truncation strategies

### 8.2 Context Window Management
- [ ] Token counting for context
- [ ] Automatic summarization when near limit
- [ ] Sliding window option

### 8.3 Shared Memory
- [ ] Key-value store across agents
- [ ] `memory::get()`, `memory::set()`
- [ ] Scoped memory (per session, global)

**Milestone:** Agent remembers previous conversation turns automatically.

---

## Phase 9: Agent Hierarchies

Priority: **LOW** - Advanced, builds on supervision.

### 9.1 Delegation
- [ ] Agent spawning sub-agents
- [ ] Budget inheritance/splitting
- [ ] Result aggregation

### 9.2 Hierarchy Patterns
- [ ] Manager/worker pattern
- [ ] Pipeline pattern (agent chains)
- [ ] Fan-out/fan-in pattern

**Milestone:** Coordinator agent delegates to specialist agents.

---

## Phase 10: Tool Sandboxing

Priority: **LOW** - Security hardening, can defer.

### 10.1 Capability Declarations
- [ ] `capabilities [ ]` syntax on tools
- [ ] `allowed_paths`, `allowed_commands` constraints

### 10.2 Runtime Enforcement
- [ ] Capability checking before tool execution
- [ ] Path validation (glob matching)
- [ ] Command whitelist checking

### 10.3 Sandboxing Implementation
- [ ] seccomp/landlock on Linux (optional)
- [ ] sandbox-exec on macOS (optional)
- [ ] Basic path checks as minimum

**Milestone:** Tool tries to read /etc/passwd, gets denied.

---

## Non-Goals (For Now)

Things we're explicitly not doing yet:
- Multi-model support (OpenAI, etc.) - Focus on Anthropic first
- Distributed execution - Single machine first
- Hot code reloading - Nice to have, not essential
- REPL - Compile-and-run is fine
- IDE/LSP support - Later
- Package manager - Way later

---

## Quick Wins

Low-effort, high-visibility improvements:
- [ ] Better error messages in parser
- [x] `--verbose` / `-v` flag for compiler
- [x] Example programs in `/examples` (hello, simple, loops, code_review)
- [x] `make run EXAMPLE=hello` convenience target
