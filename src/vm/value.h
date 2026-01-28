#ifndef VEGA_VALUE_H
#define VEGA_VALUE_H

#include "../common/memory.h"
#include <stdint.h>
#include <stdbool.h>

/*
 * Vega Runtime Values
 *
 * All values are tagged unions with reference counting for heap objects.
 */

// ============================================================================
// Value Types
// ============================================================================

typedef enum {
    VAL_NULL,
    VAL_BOOL,
    VAL_INT,
    VAL_FLOAT,
    VAL_STRING,
    VAL_AGENT,      // Agent handle
    VAL_FUTURE,     // Async result future
    VAL_ARRAY,
    VAL_RESULT,     // Result type (success or error)
    VAL_FUNCTION,   // Function reference
} ValueType;

// Forward declarations
typedef struct VegaAgent VegaAgent;
typedef struct VegaFuture VegaFuture;
typedef struct VegaArray VegaArray;
typedef struct VegaResult VegaResult;

// ============================================================================
// Value Union
// ============================================================================

typedef struct {
    ValueType type;
    union {
        bool boolean;
        int64_t integer;
        double floating;
        VegaString* string;
        VegaAgent* agent;
        VegaFuture* future;
        VegaArray* array;
        VegaResult* result;
        uint32_t function_id;
    } as;
} Value;

// ============================================================================
// Array Type
// ============================================================================

// Note: VegaObjHeader is prepended by vega_obj_alloc, not part of this struct
struct VegaArray {
    Value* items;
    uint32_t count;
    uint32_t capacity;
};

// ============================================================================
// Result Type (for error handling)
// ============================================================================

// Note: VegaObjHeader is prepended by vega_obj_alloc, not part of this struct
struct VegaResult {
    bool is_ok;
    Value value;        // Success value or error
};

// ============================================================================
// Value Constructors
// ============================================================================

static inline Value value_null(void) {
    return (Value){.type = VAL_NULL};
}

static inline Value value_bool(bool b) {
    return (Value){.type = VAL_BOOL, .as.boolean = b};
}

static inline Value value_int(int64_t i) {
    return (Value){.type = VAL_INT, .as.integer = i};
}

static inline Value value_float(double f) {
    return (Value){.type = VAL_FLOAT, .as.floating = f};
}

static inline Value value_string(VegaString* s) {
    return (Value){.type = VAL_STRING, .as.string = s};
}

static inline Value value_agent(VegaAgent* a) {
    return (Value){.type = VAL_AGENT, .as.agent = a};
}

static inline Value value_function(uint32_t id) {
    return (Value){.type = VAL_FUNCTION, .as.function_id = id};
}

// ============================================================================
// Value Operations
// ============================================================================

// Type checking
static inline bool value_is_null(Value v) { return v.type == VAL_NULL; }
static inline bool value_is_bool(Value v) { return v.type == VAL_BOOL; }
static inline bool value_is_int(Value v) { return v.type == VAL_INT; }
static inline bool value_is_float(Value v) { return v.type == VAL_FLOAT; }
static inline bool value_is_number(Value v) { return v.type == VAL_INT || v.type == VAL_FLOAT; }
static inline bool value_is_string(Value v) { return v.type == VAL_STRING; }
static inline bool value_is_agent(Value v) { return v.type == VAL_AGENT; }

// Type coercion
double value_as_number(Value v);
bool value_is_truthy(Value v);

// Comparison
bool value_equals(Value a, Value b);
int value_compare(Value a, Value b);

// Arithmetic
Value value_add(Value a, Value b);
Value value_sub(Value a, Value b);
Value value_mul(Value a, Value b);
Value value_div(Value a, Value b);
Value value_mod(Value a, Value b);
Value value_neg(Value v);

// String conversion
VegaString* value_to_string(Value v);

// Reference counting for values
void value_retain(Value v);
void value_release(Value v);

// Clone a value (deep copy for strings, shallow for primitives)
Value value_clone(Value v);

// Debug printing
void value_print(Value v);
const char* value_type_name(ValueType type);

// ============================================================================
// Array Operations
// ============================================================================

VegaArray* array_new(uint32_t initial_capacity);
void array_push(VegaArray* arr, Value v);
Value array_get(VegaArray* arr, uint32_t index);
void array_set(VegaArray* arr, uint32_t index, Value v);
uint32_t array_length(VegaArray* arr);

// ============================================================================
// Result Operations
// ============================================================================

VegaResult* result_ok(Value value);
VegaResult* result_err(Value error);
bool result_is_ok(VegaResult* r);
Value result_unwrap(VegaResult* r);
Value result_unwrap_err(VegaResult* r);
Value value_result_ok(Value value);
Value value_result_err(Value error);

#endif // VEGA_VALUE_H
