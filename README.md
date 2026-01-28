# Vega

A compiled language for fault-tolerant AI agent orchestration.

## Overview

Vega is a C-like language for building AI agent systems that don't fall over. It compiles to bytecode and runs on the Vega VM, which provides **supervision trees**, **automatic parallelization**, and **cost control** as first-class runtime features.

**Why not just use Python?** You can. But then you're implementing retries, rate limiting, error recovery, parallel execution, cost tracking, and observability yourself. Vega's runtime handles all of that so you can focus on the actual agent logic.

### Key Features

- **Supervision trees** - Agents crash. The runtime restarts them automatically (Erlang-style).
- **Automatic parallelization** - Write sequential code, get parallel API calls. Compiler analyzes dataflow.
- **Budgets as primitives** - `budget $5.00 { ... }` is a language construct, not an afterthought.
- **Built-in backpressure** - Rate limits, retries, circuit breakers handled by the VM.
- **Observability by default** - Every agent call traced. Costs, tokens, latency tracked automatically.

See [docs/SPEC.md](docs/SPEC.md) for the full language specification and [docs/TODO.md](docs/TODO.md) for the development roadmap.

## Quick Start

```bash
# Compile a Vega program
vegac agent.vega -o agent.vgb

# Run the bytecode
vega agent.vgb
```

## Example

```vega
agent Coder {
    model "claude-sonnet-4-20250514"
    system "You write clean, efficient code."
    temperature 0.3
    budget $0.50                           // Max cost per invocation

    retry {
        max_attempts 3
        backoff exponential(100ms, 2.0)
    }

    tool read_file(path: str) -> str {
        return file::read(path);
    }
}

agent Reviewer {
    model "claude-sonnet-4-20250514"
    system "You review code for bugs and style issues."
}

fn main() {
    // Spawn with supervision - auto-restart on failure
    let coder = spawn Coder supervised by {
        strategy restart
        max_restarts 3
    };
    let reviewer = spawn Reviewer;

    // Session budget - all calls share this limit
    session budget $10.00 {
        let code = coder <- "Write a function to sort a linked list";
        let review = reviewer <- code;

        if review.has("issues") {
            let fixed = coder <- "Fix these issues: " + review;
            print(fixed);
        } else {
            print(code);
        }
    }
}
```

### Automatic Parallelization

The compiler analyzes dataflow. Independent calls run in parallel automatically:

```vega
fn review_all(files: str[]) {
    // These have no data dependencies - compiler parallelizes them
    let review1 = reviewer <- files[0];
    let review2 = reviewer <- files[1];
    let review3 = reviewer <- files[2];

    // This depends on all three - waits for completion
    let summary = summarizer <- review1 + review2 + review3;
    return summary;
}
```

### Streaming

```vega
// Stream long responses chunk by chunk
for chunk in writer <~ "Write a 10-page story" {
    print(chunk);
}
```

## Status

**Early development.** The compiler frontend (lexer, parser, AST) exists. The VM and code generator are not yet implemented. See [docs/TODO.md](docs/TODO.md) for what's done and what's next.

## Building

Requirements:
- C compiler (gcc or clang)
- libcurl
- cJSON

```bash
make          # Build both vegac and vega
make vegac    # Build just the compiler
make vega     # Build just the VM
make test     # Run tests
make clean    # Clean build artifacts
```

## Environment

Set your Anthropic API key:

```bash
export ANTHROPIC_API_KEY=your-key-here
```

## License

MIT
