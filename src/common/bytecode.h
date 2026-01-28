#ifndef VEGA_BYTECODE_H
#define VEGA_BYTECODE_H

#include <stdint.h>

/*
 * Vega Bytecode Specification
 *
 * Stack-based virtual machine instruction set.
 * All instructions are 1 byte opcode + 0-3 bytes operand.
 */

// ============================================================================
// Opcodes
// ============================================================================

typedef enum {
    // Stack Operations (0x00 - 0x0F)
    OP_NOP          = 0x00,  // No operation
    OP_PUSH_CONST   = 0x01,  // Push constant from pool: [idx:u16]
    OP_PUSH_INT     = 0x02,  // Push immediate integer: [value:i32]
    OP_PUSH_TRUE    = 0x03,  // Push boolean true
    OP_PUSH_FALSE   = 0x04,  // Push boolean false
    OP_PUSH_NULL    = 0x05,  // Push null value
    OP_POP          = 0x06,  // Pop and discard top of stack
    OP_DUP          = 0x07,  // Duplicate top of stack

    // Local Variables (0x10 - 0x1F)
    OP_LOAD_LOCAL   = 0x10,  // Load local variable: [slot:u8]
    OP_STORE_LOCAL  = 0x11,  // Store to local variable: [slot:u8]
    OP_LOAD_GLOBAL  = 0x12,  // Load global variable: [idx:u16]
    OP_STORE_GLOBAL = 0x13,  // Store to global variable: [idx:u16]

    // Arithmetic (0x20 - 0x2F)
    OP_ADD          = 0x20,  // a + b (also string concat)
    OP_SUB          = 0x21,  // a - b
    OP_MUL          = 0x22,  // a * b
    OP_DIV          = 0x23,  // a / b
    OP_MOD          = 0x24,  // a % b
    OP_NEG          = 0x25,  // -a (unary negation)

    // Comparison (0x30 - 0x3F)
    OP_EQ           = 0x30,  // a == b
    OP_NE           = 0x31,  // a != b
    OP_LT           = 0x32,  // a < b
    OP_LE           = 0x33,  // a <= b
    OP_GT           = 0x34,  // a > b
    OP_GE           = 0x35,  // a >= b

    // Logical (0x40 - 0x4F)
    OP_NOT          = 0x40,  // !a
    OP_AND          = 0x41,  // a && b (short-circuit)
    OP_OR           = 0x42,  // a || b (short-circuit)

    // Control Flow (0x50 - 0x5F)
    OP_JUMP         = 0x50,  // Unconditional jump: [offset:i16]
    OP_JUMP_IF      = 0x51,  // Jump if top is truthy: [offset:i16]
    OP_JUMP_IF_NOT  = 0x52,  // Jump if top is falsy: [offset:i16]
    OP_CALL         = 0x53,  // Call function: [argc:u8]
    OP_RETURN       = 0x54,  // Return from function
    OP_CALL_NATIVE  = 0x55,  // Call native/stdlib function: [func_id:u16]

    // Agent Operations (0x60 - 0x6F)
    OP_SPAWN_AGENT  = 0x60,  // Spawn agent: [agent_id:u16] -> handle
    OP_SEND_MSG     = 0x61,  // Send message: handle, msg -> response
    OP_SPAWN_ASYNC  = 0x62,  // Spawn async agent: [agent_id:u16] -> future
    OP_AWAIT        = 0x63,  // Await future: future -> response

    // Object/Method Operations (0x70 - 0x7F)
    OP_GET_FIELD    = 0x70,  // Get field: obj, name -> value
    OP_SET_FIELD    = 0x71,  // Set field: obj, name, value -> void
    OP_CALL_METHOD  = 0x72,  // Call method: obj, name, [args] -> result

    // String Operations (0x80 - 0x8F)
    OP_STR_CONCAT   = 0x80,  // Concatenate strings: a, b -> str
    OP_STR_HAS      = 0x81,  // String contains: str, substr -> bool

    // Process/Supervision (0x90 - 0x9F)
    OP_SPAWN_PROCESS     = 0x90,  // Spawn new process: [agent_id:u16] -> pid
    OP_EXIT_PROCESS      = 0x91,  // Exit current process: [reason:u8]
    OP_YIELD             = 0x92,  // Yield to scheduler
    OP_SPAWN_SUPERVISED  = 0x93,  // Spawn with supervision: [agent_id:u16, config] -> pid
    OP_LINK              = 0x94,  // Link two processes: pid1, pid2
    OP_MONITOR           = 0x95,  // Monitor a process: pid -> monitor_ref

    // Debug/Utility (0xF0 - 0xFF)
    OP_PRINT        = 0xF0,  // Print top of stack
    OP_HALT         = 0xFF,  // Stop execution
} Opcode;

// ============================================================================
// Bytecode File Format (.vgb)
// ============================================================================

/*
 * File structure:
 *
 * +------------------+
 * | Header (16 bytes)|
 * +------------------+
 * | Constant Pool    |
 * +------------------+
 * | Agent Definitions|
 * +------------------+
 * | Function Table   |
 * +------------------+
 * | Code Section     |
 * +------------------+
 */

#define VEGA_MAGIC      0x56454741  // "VEGA" in ASCII
#define VEGA_VERSION    0x0001      // v0.1

// File header
typedef struct {
    uint32_t magic;           // VEGA_MAGIC
    uint16_t version;         // Bytecode version
    uint16_t flags;           // Reserved flags
    uint32_t const_pool_size; // Size of constant pool in bytes
    uint32_t code_size;       // Size of code section in bytes
} VegaHeader;

// Constant pool entry types
typedef enum {
    CONST_INT     = 0x01,  // 32-bit signed integer
    CONST_STRING  = 0x02,  // Length-prefixed string
    CONST_FLOAT   = 0x03,  // 64-bit float (reserved)
} ConstantType;

// Agent definition in bytecode
typedef struct {
    uint16_t name_idx;        // Index into constant pool for name
    uint16_t model_idx;       // Index into constant pool for model string
    uint16_t system_idx;      // Index into constant pool for system prompt
    uint16_t tool_count;      // Number of tools
    uint16_t temperature_x100; // Temperature * 100 (e.g., 30 = 0.3)
} AgentDef;

// Tool definition
typedef struct {
    uint16_t name_idx;        // Tool name in constant pool
    uint16_t desc_idx;        // Tool description (for Claude)
    uint16_t param_count;     // Number of parameters
    uint16_t code_offset;     // Offset to tool implementation code
} ToolDef;

// Function entry in function table
typedef struct {
    uint16_t name_idx;        // Function name in constant pool
    uint16_t param_count;     // Number of parameters
    uint16_t local_count;     // Number of local variables
    uint32_t code_offset;     // Offset into code section
    uint32_t code_length;     // Length of function code
} FunctionDef;

// ============================================================================
// Helper Macros
// ============================================================================

// Read 16-bit value from bytecode (little-endian)
#define READ_U16(code, ip) ((uint16_t)((code)[(ip)] | ((code)[(ip)+1] << 8)))

// Read 32-bit value from bytecode (little-endian)
#define READ_U32(code, ip) ((uint32_t)((code)[(ip)] | ((code)[(ip)+1] << 8) | \
                                       ((code)[(ip)+2] << 16) | ((code)[(ip)+3] << 24)))

// Read signed 16-bit value
#define READ_I16(code, ip) ((int16_t)READ_U16(code, ip))

#endif // VEGA_BYTECODE_H
