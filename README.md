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
| Observability | Add logging manually | Built-in TUI with full tracing |

## Quick Start

```bash
# Build
make

# Set your API key (one of these options)
echo 'ANTHROPIC_API_KEY=sk-...' >> ~/.vega   # Option 1: config file (recommended)
export ANTHROPIC_API_KEY=sk-...              # Option 2: environment variable

# Compile and run
./bin/vegac examples/hello.vega -o hello.vgb
./bin/vega hello.vgb
```

## Interactive TUI

Vega includes a full terminal UI for visualizing agent execution in real-time:

```bash
# Launch TUI with a program
./bin/vega tui program.vgb

# Or launch interactively and load programs via REPL
./bin/vega tui
```

The TUI provides:
- **Agents panel**: Track spawned agents, models, and token usage
- **Messages panel**: View conversation history for selected agent
- **Tools panel**: Monitor tool calls and results
- **Logs panel**: Real-time HTTP requests, timing, and token counts
- **REPL**: Load programs, run commands, inspect state

```
+------------------------------------------------------------------+
| Vega TUI                                 In:1.2K Out:892 [F1:Help]|
+---------------------------+--------------------------------------+
| AGENTS                    | MESSAGES                             |
| > Coder                   | USER: Write a sort function          |
|   claude-sonnet-4-20250514|                                      |
|   tok: 2.1K               | ASST: Here's an efficient quicksort  |
+---------------------------+--------------------------------------+
| TOOLS                     | LOGS                                 |
| read_file                 | [10:23:45] HTTP 200 (1.2s) in:523    |
|   -> src/main.c           | [10:23:46] Agent spawned: Coder      |
|   <- 1.2KB                | [10:23:48] <- Coder (2.1s)           |
+---------------------------+--------------------------------------+
| > _                                                              |
+------------------------------------------------------------------+
```

**Keys**: Tab (switch panels), Up/Down (navigate), F1 (help), F10 (quit)

**Commands**: `load <file>`, `run`, `agents`, `clear`, `help`, `quit`

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

### Imports

Vega supports importing code from other files:

```vega
import "math";              // Import from stdlib
import "string";            // Import from stdlib
import "./helpers";         // Relative import (same directory)
import "../utils/common";   // Relative import (parent directory)

fn main() {
    print(max(10, 20));      // From math
    print(repeat("*", 5));   // From string
}
```

**Search path resolution:**
1. Relative paths (`./foo`, `../bar`) resolve from the current file
2. Other paths resolve from `./stdlib/` in the working directory
3. Set `VEGA_PATH` environment variable for custom locations

## Standard Library

Vega includes a standard library in the `stdlib/` directory.

### math

```vega
import "math";

max(a: int, b: int) -> int      // Larger of two values
min(a: int, b: int) -> int      // Smaller of two values
abs(n: int) -> int              // Absolute value
clamp(val, lo, hi: int) -> int  // Constrain to range
sign(n: int) -> int             // -1, 0, or 1
```

### string

```vega
import "string";

len(s: str) -> int                        // String length
contains(s: str, substr: str) -> bool     // Check substring
char_at(s: str, index: int) -> str        // Get character
repeat(s: str, n: int) -> str             // Repeat string
pad_left(s: str, width: int, pad: str)    // Left pad
pad_right(s: str, width: int, pad: str)   // Right pad
```

### Native Functions

These are built into the VM and always available (no import needed):

```vega
// File I/O
file::read(path: str) -> str
file::write(path: str, content: str)
file::exists(path: str) -> bool
file::list(dir: str) -> str

// String utilities
str::len(s: str) -> int
str::contains(s: str, substr: str) -> bool
str::char_at(s: str, index: int) -> str
str::split(s: str, delim: str) -> str[]

// Output
print(value)   // Print any value
```

## Project Structure

```
src/
  compiler/    # Lexer, parser, semantic analysis, code generation
  vm/          # Bytecode interpreter, agent runtime, scheduler
  tui/         # Terminal UI and tracing system
  common/      # Shared utilities (memory management, bytecode spec)
stdlib/        # Standard library (math, string, etc.)
examples/      # Example programs
docs/          # Language specification and design docs
```

## Configuration

Vega looks for your Anthropic API key in two places (in order):

1. **Environment variable**: `ANTHROPIC_API_KEY`
2. **Config file**: `~/.vega`

The config file uses simple `key=value` format:

```bash
# ~/.vega
ANTHROPIC_API_KEY=sk-ant-api03-...
```

## Building

Requirements:
- C compiler (gcc or clang)
- libcurl
- ncurses (for TUI)
- make

```bash
make              # Build everything
make clean        # Clean build artifacts
make release      # Optimized build
make run EXAMPLE=hello   # Compile and run an example
make tui EXAMPLE=hello   # Run example in TUI mode
```

### Cross-Compilation for Linux

Build Linux binaries from macOS using Docker:

```bash
make linux        # Outputs to bin-linux/
```

### CI/CD

GitHub Actions automatically builds and tests on Linux and macOS. Release artifacts are attached when you create a GitHub release:

```bash
gh release create v0.1.0 --title "Vega v0.1.0"
```

## Status

The core language is functional:
- Compiler (lexer, parser, semantic analysis, code generation)
- VM (bytecode interpreter, stack machine)
- Agent runtime (spawn, message passing, tool execution)
- Supervision (process model, restart strategies)
- Anthropic API integration
- Import system and standard library
- Interactive TUI with tracing and REPL

See [docs/TODO.md](docs/TODO.md) for the roadmap.

## License

MIT
