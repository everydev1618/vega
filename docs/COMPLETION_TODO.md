# Vega Language Completion TODO

**Current Status: 4/20 tests passing (Early Alpha)**

This document tracks what language features need to be implemented to achieve full completeness, based on the test suite in `tests/completions/`.

---

## Test Results Summary

| Test | Name | Status | Blocker |
|------|------|--------|---------|
| 01 | Hello World | PASS | - |
| 02 | FizzBuzz | PASS | - |
| 03 | Fibonacci (Recursive) | PASS | - |
| 04 | Factorial | PASS | - |
| 05 | String Reversal | FAIL | `str::char_at()` not implemented |
| 06 | Array Operations | FAIL | Array literals `[]` not parseable |
| 07 | File Read/Write | FAIL | `str::char_at()` needed for newline counting |
| 08 | JSON Parse | FAIL | `json::get_string()` etc. not exposed to VM |
| 09 | HTTP GET Request | FAIL | `http::get()` not implemented |
| 10 | Error Handling | FAIL | `Result<T,E>` generics and `match` not supported |
| 11 | Spawn Single Agent | PASS | Agent spawning and messaging works |
| 12 | Agent with Tool | FAIL | Tool calling may not work correctly |
| 13 | Two Agents Sequence | PASS | Multi-agent pipelines work |
| 14 | Parallel Agents | FAIL | `spawn async` syntax not parsed |
| 15 | Agent Conversation Loop | FAIL | Loop with agent may timeout/hang |
| 16 | Prime Sieve | FAIL | Array indexing `arr[i]` not supported |
| 17 | Binary Search | FAIL | Array indexing not supported |
| 18 | Merge Sort | FAIL | Array operations not supported |
| 19 | Word Frequency | FAIL | Arrays and `str::split()` not supported |
| 20 | Simple Calculator | FAIL | Arrays and `str::split()` not supported |

---

## Priority 1: Array Support (Unblocks 6 tests)

**Tests blocked:** 06, 16, 17, 18, 19, 20

### 1.1 Array Literals
```vega
let arr: int[] = [1, 2, 3, 4, 5];
let empty: int[] = [];
```

**Parser changes needed:**
- Add `LBRACKET` (`[`) and `RBRACKET` (`]`) token handling in expression parsing
- Parse comma-separated expressions between brackets
- Create `AST_ARRAY_LITERAL` node type

**Codegen changes needed:**
- Emit `OP_ARRAY_NEW` with element count
- Emit code for each element, then `OP_ARRAY_PUSH` or similar

### 1.2 Array Indexing
```vega
let x = arr[0];      // Read
arr[0] = 42;         // Write
```

**Parser changes needed:**
- Handle postfix `[expr]` after identifiers
- Create `AST_INDEX` node for read access
- Create `AST_INDEX_ASSIGN` node for write access

**VM changes needed:**
- `OP_ARRAY_GET` - pop index, pop array, push element
- `OP_ARRAY_SET` - pop value, pop index, pop array, set element

### 1.3 Array Concatenation
```vega
let combined = arr1 + arr2;
let appended = arr + [newElement];
```

**VM changes needed:**
- Handle `OP_ADD` for array types
- Create new array with combined elements

---

## Priority 2: String Functions (Unblocks 4 tests)

**Tests blocked:** 05, 07, 19, 20

### 2.1 `str::char_at(s: str, index: int) -> str`
Returns single character at index position.

**Implementation in `vm.c` `call_native()`:**
```c
if (strcmp(name, "str::char_at") == 0 && argc == 2) {
    if (args[0].type != VAL_STRING || args[1].type != VAL_INT) {
        return value_string("");
    }
    ClawString* s = args[0].as.string;
    int64_t idx = args[1].as.integer;
    if (idx < 0 || idx >= s->length) {
        return value_string("");
    }
    char buf[2] = {s->data[idx], '\0'};
    return value_string(buf);
}
```

### 2.2 `str::from_int(n: int) -> str`
Converts integer to string.

```c
if (strcmp(name, "str::from_int") == 0 && argc == 1) {
    if (args[0].type != VAL_INT) return value_string("");
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)args[0].as.integer);
    return value_string(buf);
}
```

### 2.3 `str::split(s: str, delimiter: str) -> str[]`
Splits string into array of strings.

**Requires:** Array support (Priority 1)

### 2.4 `str::char_code(c: str) -> int`
Returns ASCII code of first character.

### 2.5 `str::char_lower(c: str) -> str`
Converts character to lowercase.

---

## Priority 3: JSON Standard Library (Unblocks 1 test)

**Tests blocked:** 08

The JSON parsing code exists in `src/stdlib/json.c` but isn't exposed to the language.

### 3.1 Expose JSON functions in `call_native()`:
- `json::get_string(json: str, key: str) -> str`
- `json::get_int(json: str, key: str) -> int`
- `json::get_float(json: str, key: str) -> float`
- `json::get_bool(json: str, key: str) -> bool`
- `json::get_array(json: str, key: str) -> str` (returns JSON array as string)
- `json::array_len(json_array: str) -> int`
- `json::array_get(json_array: str, index: int) -> str`

---

## Priority 4: HTTP Standard Library (Unblocks 1 test)

**Tests blocked:** 09

### 4.1 `http::get(url: str) -> str`
Makes HTTP GET request, returns response body.

**Implementation:** Use libcurl (already linked) in `call_native()`.

### 4.2 `http::post(url: str, body: str) -> str`
Makes HTTP POST request with body.

---

## Priority 5: Result Type & Pattern Matching (Unblocks 1 test)

**Tests blocked:** 10

This is a significant language feature addition.

### 5.1 Result Type
```vega
fn divide(a: int, b: int) -> Result<int, str> {
    if b == 0 {
        return Err("division by zero");
    }
    return Ok(a / b);
}
```

**Type system changes:**
- Add `TYPE_RESULT` with two type parameters
- Add `Ok(value)` and `Err(value)` constructors

### 5.2 Match Expression
```vega
match result {
    Ok(value) => print(value),
    Err(msg) => print(msg)
}
```

**Parser changes:**
- Add `match` keyword
- Parse match arms with `=>` syntax
- Support destructuring bindings

---

## Priority 6: Async Agents (Unblocks 1 test)

**Tests blocked:** 14

### 6.1 `spawn async`
```vega
let worker = spawn Worker async;
```

Spawns agent without blocking.

### 6.2 `await`
```vega
let response = worker <- "message";
await response;
```

Waits for async operation to complete.

**Implementation:** Use the existing process/scheduler infrastructure in `src/vm/process.c` and `src/vm/scheduler.c`.

---

## Implementation Order

Recommended order to maximize test pass rate:

1. **Array literals and indexing** → +6 tests (06, 16, 17, 18, 19, 20)
2. **String functions (char_at, from_int)** → +2 tests (05, 07)
3. **String split (requires arrays)** → enables 19, 20 to fully work
4. **JSON stdlib exposure** → +1 test (08)
5. **HTTP stdlib** → +1 test (09)
6. **Result type + match** → +1 test (10)
7. **Async spawn + await** → +1 test (14)

---

## Quick Wins

These can be done in under an hour each:

1. **`str::char_at()`** - ~20 lines in `vm.c`
2. **`str::from_int()`** - ~10 lines in `vm.c`
3. **JSON function exposure** - Wire existing `src/stdlib/json.c` functions to `call_native()`
4. **`http::get()`** - Use existing `src/vm/http.c` infrastructure

---

## Stretch Goals

Features mentioned in CLAUDE.md but not yet tested:

- [ ] Streaming send operator `<~`
- [ ] Supervision trees (`spawn X supervised by { ... }`)
- [ ] Budget blocks (`budget $5.00 { ... }`)
- [ ] Try/catch error handling
- [ ] File listing `file::list(dir)`

---

## Running Tests

```bash
# Run all tests
./tests/completions/run_tests.sh

# Run with agent tests (requires API key)
ANTHROPIC_API_KEY=your_key ./tests/completions/run_tests.sh
```

**Scoring:**
- 20/20: Vega is complete
- 15-19: Vega is usable
- 10-14: Vega is in progress
- 1-9: Vega is early alpha
- 0: Vega doesn't run
