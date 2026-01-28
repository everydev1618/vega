# Vega Project

This project uses **Vega**, a language for building AI agent systems.

## Quick Reference

```bash
vegac program.vega -o program.vgb   # Compile
claw program.vgb                     # Run (needs ANTHROPIC_API_KEY)
```

## Syntax Cheatsheet

```vega
// Types: int, float, str, bool, null, type[]

// Variables
let x = 42;
let name: str = "Alice";

// Functions
fn add(a: int, b: int) -> int {
    return a + b;
}

// Agents
agent Helper {
    model "claude-sonnet-4-20250514"
    system "You are helpful."
    temperature 0.7

    tool read_file(path: str) -> str {
        return file::read(path);
    }
}

// Main entry point
fn main() {
    let helper = spawn Helper;
    let response = helper <- "Hello!";
    print(response);

    // String methods
    if response.has("keyword") {
        print("Found it");
    }

    // Loops
    let i = 0;
    while i < 5 {
        print(i);
        i = i + 1;
    }

    for let j = 0; j < 5; j {
        print(j);
        j = j + 1;
    }
}
```

## Key Points

- Agents wrap Claude API with config (model, system prompt, temperature, tools)
- `spawn Agent` creates an agent handle
- `agent <- "message"` sends a message and waits for response
- Agent conversations persist (maintains message history)
- Use `.has("substring")` to check agent responses
- `+` concatenates strings
- `print()` is the output function
