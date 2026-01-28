# Vega Language Project

Vega is a compiled, statically-typed language for building fault-tolerant AI agent systems. It compiles to bytecode and runs on the Vega VM.

## Build & Run

```bash
# Build compiler and VM
make

# Compile a program
./bin/vegac program.vega -o program.vgb

# Run it (requires ANTHROPIC_API_KEY environment variable)
./bin/vega program.vgb

# Or use make shortcut
make run EXAMPLE=hello
```

## Project Structure

```
src/compiler/    # Compiler (lexer, parser, sema, codegen)
src/vm/          # Runtime (VM, agents, processes, scheduler)
examples/        # Example Vega programs
docs/            # Language documentation
```

---

## Language Reference

### Data Types

| Type | Description | Example |
|------|-------------|---------|
| `int` | Integer | `42`, `-17` |
| `float` | Floating point | `3.14`, `-0.5` |
| `str` | String | `"hello"` |
| `bool` | Boolean | `true`, `false` |
| `null` | Null value | `null` |
| `type[]` | Array | `int[]`, `str[]` |
| `agent` | Agent handle | returned by `spawn` |

### Variable Declaration

```vega
let x = 42;              // Type inferred
let name: str = "Alice"; // Explicit type
let values: int[] = [1, 2, 3];
```

### Operators

```vega
// Arithmetic
+ - * / %

// Comparison
== != < > <= >=

// Logical
&& || !

// String
+ (concatenation)
.has("substring")  // contains check

// Agent messaging
<-  // synchronous send
<~  // streaming send (planned)
```

### Control Flow

```vega
// If/else
if condition {
    // ...
} else if other {
    // ...
} else {
    // ...
}

// While loop
while condition {
    // ...
    if done { break; }
    if skip { continue; }
}

// For loop
for let i = 0; i < 10; i {
    // ...
    i = i + 1;
}
```

### Functions

```vega
fn add(a: int, b: int) -> int {
    return a + b;
}

fn greet(name: str) {
    print("Hello, " + name);
}

fn main() {
    let result = add(10, 20);
    greet("World");
}
```

---

## Agents

Agents are the core abstraction - they wrap Claude API calls with configuration.

### Agent Declaration

```vega
agent Coder {
    model "claude-sonnet-4-20250514"    // Required: model ID
    system "You write clean code."       // Required: system prompt
    temperature 0.3                      // Optional: 0.0-1.0

    // Optional: tools the agent can call
    tool read_file(path: str) -> str {
        return file::read(path);
    }

    tool list_files(dir: str) -> str {
        return file::list(dir);
    }
}
```

### Spawning Agents

```vega
// Basic spawn
let coder = spawn Coder;

// With supervision (planned)
let coder = spawn Coder supervised by {
    strategy restart
    max_restarts 3
    window 60s
};
```

### Sending Messages

```vega
// Synchronous send - blocks until response
let response = agent <- "Write a function to sort an array";

// The response is a string containing Claude's reply
print(response);

// Chain conversations (agent maintains history)
let code = coder <- "Write a prime checker in Python";
let review = coder <- "Now add error handling to that code";
```

---

## Standard Library

### Print

```vega
print("Hello");      // Print string
print(42);           // Print int
print(3.14);         // Print float
print(true);         // Print bool
```

### File Operations (in agent tools)

```vega
file::read(path: str) -> str      // Read file contents
file::write(path: str, content: str)  // Write file
file::exists(path: str) -> bool   // Check if exists
file::list(dir: str) -> str       // List directory
```

### HTTP (planned)

```vega
http::get(url: str) -> str
http::post(url: str, body: str) -> str
```

---

## Common Patterns

### Simple Single Agent

```vega
agent Assistant {
    model "claude-sonnet-4-20250514"
    system "You are a helpful assistant."
    temperature 0.7
}

fn main() {
    let assistant = spawn Assistant;
    let response = assistant <- "What is the capital of France?";
    print(response);
}
```

### Multi-Agent Pipeline

```vega
agent Writer {
    model "claude-sonnet-4-20250514"
    system "You write content. Be creative and engaging."
    temperature 0.8
}

agent Editor {
    model "claude-sonnet-4-20250514"
    system "You edit content for clarity and grammar. Return the improved version."
    temperature 0.3
}

fn main() {
    let writer = spawn Writer;
    let editor = spawn Editor;

    let draft = writer <- "Write a short story about a robot learning to paint";
    print("=== Draft ===");
    print(draft);

    let final = editor <- "Edit this story:\n" + draft;
    print("=== Final ===");
    print(final);
}
```

### Agent with Tools

```vega
agent Researcher {
    model "claude-sonnet-4-20250514"
    system "You research topics using available tools. Read files to gather information."
    temperature 0.3

    tool read_file(path: str) -> str {
        return file::read(path);
    }

    tool list_dir(path: str) -> str {
        return file::list(path);
    }
}

fn main() {
    let researcher = spawn Researcher;
    let analysis = researcher <- "Analyze the code in src/main.c and summarize what it does";
    print(analysis);
}
```

### Conditional Logic with Agent Output

```vega
agent Classifier {
    model "claude-sonnet-4-20250514"
    system "Classify sentiment as POSITIVE, NEGATIVE, or NEUTRAL. Reply with only the classification."
    temperature 0.0
}

fn main() {
    let classifier = spawn Classifier;
    let sentiment = classifier <- "I love this product! Best purchase ever.";

    if sentiment.has("POSITIVE") {
        print("Customer is happy!");
    } else if sentiment.has("NEGATIVE") {
        print("Customer is unhappy, escalate to support.");
    } else {
        print("Neutral feedback.");
    }
}
```

### Iterative Refinement

```vega
agent Coder {
    model "claude-sonnet-4-20250514"
    system "You write Python code. Return only code, no explanations."
    temperature 0.3
}

agent Reviewer {
    model "claude-sonnet-4-20250514"
    system "Review code for bugs. Say APPROVED if good, otherwise list issues."
    temperature 0.2
}

fn main() {
    let coder = spawn Coder;
    let reviewer = spawn Reviewer;

    let code = coder <- "Write a function to find the longest palindrome in a string";
    let review = reviewer <- code;

    let attempts = 0;
    while !review.has("APPROVED") {
        if attempts >= 3 {
            print("Max attempts reached");
            break;
        }

        print("Revision needed...");
        code = coder <- "Fix these issues:\n" + review + "\n\nOriginal code:\n" + code;
        review = reviewer <- code;
        attempts = attempts + 1;
    }

    print("=== Final Code ===");
    print(code);
}
```

---

## Error Handling (Planned)

```vega
try {
    let result = agent <- task;
} catch BudgetExceeded {
    print("Out of budget");
} catch AgentFailed(reason) {
    print("Agent failed: " + reason);
}
```

## Budgets (Planned)

```vega
// Limit spending for a block
budget $5.00 {
    let response = agent <- expensive_task;
}

// Query remaining budget
let remaining = budget::remaining();
```

---

## Tips for Claude Code

When writing Vega programs:

1. **Always declare agents before `fn main()`**
2. **Spawn agents inside functions**, not at top level
3. **Use `.has()` for string matching** on agent responses
4. **Agent conversations persist** - subsequent messages include history
5. **Print is the only output** - use it for debugging and results
6. **String concatenation uses `+`** - build prompts by concatenating
7. **No semicolon after blocks** - `if {} else {}` not `if {}; else {};`
8. **For loops need manual increment** - `i = i + 1` inside the loop
