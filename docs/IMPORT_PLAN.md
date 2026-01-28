# Import System Implementation Plan

This document outlines how to add an import system to Vega, enabling code reuse and a Vega-native standard library.

## Design Decisions

**Syntax:**
```vega
import "path/to/module"           // Import all exports
import "path/to/module" as mod    // Import with namespace alias
```

**Resolution:**
- Relative paths: `import "./utils"` → same directory as current file
- Stdlib paths: `import "stdlib/math"` → `$VEGA_HOME/stdlib/math.vega`
- No file extension needed (assumes `.vega`)

**Semantics:**
- Compile-time resolution (no runtime module loading)
- Imported functions/agents become available in current scope
- Circular imports detected and rejected
- Each file compiled once, cached

---

## Phase 1: Lexer & AST

### 1.1 Add `import` keyword

**src/compiler/lexer.h** - Add token type:
```c
// Around line 49, with other keywords
TOK_IMPORT,
TOK_AS,        // for "import X as Y"
```

**src/compiler/lexer.c** - Add to keywords array:
```c
{"import",      TOK_IMPORT},
{"as",          TOK_AS},
```

### 1.2 Add Import AST node

**src/compiler/ast.h** - Add declaration kind:
```c
typedef enum {
    DECL_AGENT,
    DECL_FUNCTION,
    DECL_TOOL,
    DECL_IMPORT,    // NEW
} AstDeclKind;
```

**src/compiler/ast.h** - Add import struct:
```c
typedef struct {
    char* path;         // "stdlib/math" or "./utils"
    char* alias;        // Optional namespace alias
    SourceLoc loc;
} ImportDecl;
```

**src/compiler/ast.h** - Add to AstDecl union:
```c
typedef struct AstDecl {
    AstDeclKind kind;
    SourceLoc loc;
    union {
        AgentDecl agent;
        FunctionDecl function;
        ToolDecl tool;
        ImportDecl import;    // NEW
    } as;
} AstDecl;
```

---

## Phase 2: Parser

**src/compiler/parser.c** - Modify `parse_declaration()`:
```c
static AstDecl* parse_declaration(Parser* parser) {
    // Handle imports first (must be at top of file)
    if (match(parser, TOK_IMPORT)) {
        return parse_import(parser);
    }
    if (match(parser, TOK_AGENT)) {
        return parse_agent(parser);
    }
    if (match(parser, TOK_FN)) {
        return parse_function(parser);
    }
    // ...
}
```

**src/compiler/parser.c** - Add `parse_import()`:
```c
static AstDecl* parse_import(Parser* parser) {
    SourceLoc loc = parser->previous.loc;

    // Expect string path
    consume(parser, TOK_STRING, "Expected module path after 'import'");
    char* path = strndup(
        parser->previous.start + 1,  // skip opening quote
        parser->previous.length - 2   // exclude both quotes
    );

    // Optional alias: import "foo" as bar
    char* alias = NULL;
    if (match(parser, TOK_AS)) {
        consume(parser, TOK_IDENT, "Expected alias name after 'as'");
        alias = strndup(parser->previous.start, parser->previous.length);
    }

    consume(parser, TOK_SEMICOLON, "Expected ';' after import");

    // Build AST node
    AstDecl* decl = malloc(sizeof(AstDecl));
    decl->kind = DECL_IMPORT;
    decl->loc = loc;
    decl->as.import.path = path;
    decl->as.import.alias = alias;
    decl->as.import.loc = loc;

    return decl;
}
```

---

## Phase 3: Semantic Analysis

This is the most complex phase. We need to:
1. Resolve import paths to actual files
2. Parse and analyze imported files (recursively)
3. Detect circular imports
4. Merge imported symbols into current scope

### 3.1 Module cache

**src/compiler/sema.h** - Add module tracking:
```c
typedef struct Module {
    char* path;                 // Canonical path
    AstProgram* ast;            // Parsed AST
    Scope* exports;             // Exported symbols
    bool analyzing;             // For circular detection
    bool analyzed;              // Already processed
    struct Module* next;        // Hash chain
} Module;

typedef struct {
    Module** modules;           // Hash table of loaded modules
    uint32_t capacity;
    char* search_paths[8];      // Where to look for modules
    uint32_t search_path_count;
} ModuleCache;
```

### 3.2 Path resolution

**src/compiler/sema.c** - Add resolver:
```c
// Resolve import path to actual file path
static char* resolve_import_path(Sema* sema, const char* import_path,
                                  const char* current_file) {
    char resolved[1024];

    // Relative import: "./foo" or "../foo"
    if (import_path[0] == '.') {
        // Get directory of current file
        char* dir = dirname(strdup(current_file));
        snprintf(resolved, sizeof(resolved), "%s/%s.vega", dir, import_path);
        free(dir);

        if (file_exists(resolved)) {
            return realpath(resolved, NULL);
        }
    }

    // Search in stdlib and search paths
    for (uint32_t i = 0; i < sema->modules.search_path_count; i++) {
        snprintf(resolved, sizeof(resolved), "%s/%s.vega",
                 sema->modules.search_paths[i], import_path);
        if (file_exists(resolved)) {
            return realpath(resolved, NULL);
        }
    }

    return NULL;  // Not found
}
```

### 3.3 Import processing

**src/compiler/sema.c** - Process imports before other declarations:
```c
bool sema_analyze(Sema* sema, AstProgram* program, const char* source_path) {
    // Phase 0: Process imports first
    for (uint32_t i = 0; i < program->decl_count; i++) {
        if (program->decls[i]->kind == DECL_IMPORT) {
            if (!process_import(sema, &program->decls[i]->as.import, source_path)) {
                return false;
            }
        }
    }

    // Phase 1: Register declarations (existing)
    register_declarations(sema, program);

    // Phase 2: Analyze bodies (existing)
    // ...
}

static bool process_import(Sema* sema, ImportDecl* import, const char* current_file) {
    // Resolve path
    char* resolved = resolve_import_path(sema, import->path, current_file);
    if (!resolved) {
        sema_error(sema, import->loc, "Cannot find module '%s'", import->path);
        return false;
    }

    // Check if already loaded
    Module* mod = module_cache_get(&sema->modules, resolved);
    if (mod) {
        if (mod->analyzing) {
            sema_error(sema, import->loc, "Circular import detected: %s", import->path);
            free(resolved);
            return false;
        }
        // Already analyzed, just import symbols
        import_symbols(sema, mod, import->alias);
        free(resolved);
        return true;
    }

    // Load and analyze new module
    mod = load_module(sema, resolved);
    if (!mod) {
        free(resolved);
        return false;
    }

    // Import symbols into current scope
    import_symbols(sema, mod, import->alias);
    free(resolved);
    return true;
}
```

### 3.4 Symbol importing

```c
static void import_symbols(Sema* sema, Module* mod, const char* alias) {
    // Iterate through module's exported symbols
    for (uint32_t i = 0; i < mod->exports->capacity; i++) {
        for (Symbol* sym = mod->exports->symbols[i]; sym; sym = sym->next) {
            if (alias) {
                // Namespaced: import "foo" as f → f::bar
                char namespaced[256];
                snprintf(namespaced, sizeof(namespaced), "%s::%s", alias, sym->name);
                scope_add(sema->current_scope, namespaced, sym->kind, sym->type);
            } else {
                // Direct: import "foo" → bar
                scope_add(sema->current_scope, sym->name, sym->kind, sym->type);
            }
        }
    }
}
```

---

## Phase 4: Code Generation

For compile-time imports, codegen needs to:
1. Track which functions/agents come from imports
2. Ensure imported code is emitted once
3. Handle function ID remapping across modules

### 4.1 Multi-module codegen

**src/compiler/codegen.c**:
```c
bool codegen_generate(CodeGen* cg, AstProgram* program, ModuleCache* modules) {
    // First: emit all imported modules
    for (Module* mod = modules->first; mod; mod = mod->next) {
        if (!mod->emitted) {
            emit_module(cg, mod);
            mod->emitted = true;
        }
    }

    // Then: emit main program
    // (existing agent/function emission)
}
```

### 4.2 Function ID remapping

When importing, function IDs need to be adjusted:
```c
typedef struct {
    uint32_t original_id;
    uint32_t remapped_id;
} FunctionRemap;

// During emit, remap function calls to imported functions
```

---

## Phase 5: Standard Library

Create `stdlib/` directory with Vega implementations:

```
stdlib/
  math.vega      # math utilities
  string.vega    # string helpers
  array.vega     # array utilities
  io.vega        # I/O helpers (wraps native file::)
```

**stdlib/math.vega**:
```vega
fn max(a: int, b: int) -> int {
    if a > b { return a; }
    return b;
}

fn min(a: int, b: int) -> int {
    if a < b { return a; }
    return b;
}

fn abs(n: int) -> int {
    if n < 0 { return 0 - n; }
    return n;
}

fn clamp(val: int, lo: int, hi: int) -> int {
    if val < lo { return lo; }
    if val > hi { return hi; }
    return val;
}
```

**stdlib/string.vega**:
```vega
fn repeat(s: str, n: int) -> str {
    let result = "";
    let i = 0;
    while i < n {
        result = result + s;
        i = i + 1;
    }
    return result;
}

fn starts_with(s: str, prefix: str) -> bool {
    // Use native str:: functions
    let prefix_len = str::len(prefix);
    let s_len = str::len(s);
    if prefix_len > s_len { return false; }

    let i = 0;
    while i < prefix_len {
        if str::char_at(s, i) != str::char_at(prefix, i) {
            return false;
        }
        i = i + 1;
    }
    return true;
}
```

---

## Implementation Order

1. **Week 1: Lexer + Parser**
   - Add TOK_IMPORT, TOK_AS tokens
   - Add ImportDecl AST node
   - Add parse_import() function
   - Test: `--ast` flag shows import nodes

2. **Week 2: Basic Semantic Analysis**
   - Path resolution (relative paths only)
   - Module cache structure
   - Circular import detection
   - Test: importing a simple module works

3. **Week 3: Code Generation**
   - Multi-module emission
   - Function ID remapping
   - Test: imported functions callable at runtime

4. **Week 4: Stdlib + Polish**
   - Create stdlib/ directory
   - Add search path for stdlib
   - Write initial stdlib modules (math, string)
   - Test: `import "stdlib/math"` works

---

## Example Usage

After implementation:

```vega
import "stdlib/math";
import "./helpers" as h;

agent Calculator {
    model "claude-sonnet-4-20250514"
    system "You are a calculator."
}

fn main() {
    let a = 10;
    let b = 3;

    print(max(a, b));        // From stdlib/math
    print(h::double(a));     // From ./helpers with alias

    let calc = spawn Calculator;
    let result = calc <- "What is 2 + 2?";
    print(result);
}
```

**./helpers.vega**:
```vega
fn double(n: int) -> int {
    return n * 2;
}

fn triple(n: int) -> int {
    return n * 3;
}
```
