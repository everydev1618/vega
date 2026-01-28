# Vega

**The programming language for the agentic era.**

Vega is a statically-typed, compiled language purpose-built for AI agents. It compiles to bytecode and runs on the Vega VM, where fault tolerance, cost control, and observability are first-class concerns—not afterthoughts.

```vega
agent Coder {
    model "claude-sonnet-4-20250514"
    system "You write clean, efficient code."
    temperature 0.3

    tool read_file(path: str) -> str {
        return file::read(path);
    }
}

fn main() {
    let coder = spawn Coder supervised by {
        strategy restart
        max_restarts 3
    };

    let code = coder <- "Write a function to sort a linked list";
    print(code);
}
```

## Why Vega?

Building reliable AI agent systems in Python means implementing retries, rate limiting, error recovery, cost tracking, and observability yourself. Every team rebuilds the same infrastructure.

Vega's runtime handles all of that:

| Problem | Python + SDK | Vega |
|---------|--------------|------|
| Agent crashes | Try/except everywhere | Supervision trees auto-restart |
| Rate limits | Manual backoff logic | Built into the VM |
| Cost overruns | Hope for the best | `budget $5.00 { ... }` |
| Parallel calls | asyncio complexity | Automatic dataflow analysis |
| Observability | Add logging manually | Every call traced by default |

## Quick Start

```bash
# Build
make

# Set your API key
export ANTHROPIC_API_KEY=sk-...

# Compile and run
./bin/vegac examples/hello.vega -o hello.vgb
./bin/vega hello.vgb
```

## Language Overview

### Agents

Agents wrap LLM calls with configuration. They maintain conversation history automatically.

```vega
agent Assistant {
    model "claude-sonnet-4-20250514"
    system "You are a helpful assistant."
    temperature 0.7
}

fn main() {
    let assistant = spawn Assistant;

    // Send a message, get a response
    let response = assistant <- "What is the capital of France?";
    print(response);

    // Conversation history is maintained
    let followup = assistant <- "What's the population?";
    print(followup);
}
```

### Tools

Agents can call back into Vega code:

```vega
agent Researcher {
    model "claude-sonnet-4-20250514"
    system "Research topics using available tools."

    tool read_file(path: str) -> str {
        return file::read(path);
    }

    tool list_dir(path: str) -> str {
        return file::list(path);
    }
}

fn main() {
    let researcher = spawn Researcher;
    let analysis = researcher <- "Summarize the code in src/main.c";
    print(analysis);
}
```

### Supervision

Agents fail. Networks timeout. APIs rate-limit. Vega handles this with Erlang-style supervision:

```vega
fn main() {
    // If this agent crashes, restart it (up to 3 times per minute)
    let worker = spawn Worker supervised by {
        strategy restart
        max_restarts 3
        window 60000
    };

    // Use the agent normally - failures are handled automatically
    let result = worker <- "Do something that might fail";
    print(result);
}
```

### Control Flow

Standard imperative constructs:

```vega
fn factorial(n: int) -> int {
    if n <= 1 {
        return 1;
    }
    return n * factorial(n - 1);
}

fn main() {
    // While loops
    let i = 0;
    while i < 5 {
        print(factorial(i));
        i = i + 1;
    }

    // For loops
    for let j = 0; j < 5; j {
        print(j);
        j = j + 1;
    }
}
```

### String Operations

```vega
fn main() {
    let text = "Hello, World!";

    // Check if string contains substring
    if text.has("World") {
        print("Found it!");
    }

    // Concatenation
    let greeting = "Hello, " + "Vega!";
    print(greeting);
}
```

## Project Structure

```
src/
  compiler/    # Lexer, parser, semantic analysis, code generation
  vm/          # Bytecode interpreter, agent runtime, scheduler
  common/      # Shared utilities (memory management, bytecode spec)
  stdlib/      # Standard library (file I/O, strings, JSON)
examples/      # Example programs
docs/          # Language specification and design docs
```

## Building

Requirements:
- C compiler (gcc or clang)
- libcurl
- make

```bash
make              # Build everything
make clean        # Clean build artifacts
make run EXAMPLE=hello   # Compile and run an example
```

## Status

The core language is functional:
- Compiler (lexer, parser, semantic analysis, code generation)
- VM (bytecode interpreter, stack machine)
- Agent runtime (spawn, message passing, tool execution)
- Supervision (process model, restart strategies)
- Anthropic API integration

See [docs/TODO.md](docs/TODO.md) for the roadmap.

## License

MIT
