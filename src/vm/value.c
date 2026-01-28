#include "value.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ============================================================================
// Type Coercion
// ============================================================================

double value_as_number(Value v) {
    switch (v.type) {
        case VAL_INT:   return (double)v.as.integer;
        case VAL_FLOAT: return v.as.floating;
        case VAL_BOOL:  return v.as.boolean ? 1.0 : 0.0;
        default:        return 0.0;
    }
}

bool value_is_truthy(Value v) {
    switch (v.type) {
        case VAL_NULL:   return false;
        case VAL_BOOL:   return v.as.boolean;
        case VAL_INT:    return v.as.integer != 0;
        case VAL_FLOAT:  return v.as.floating != 0.0;
        case VAL_STRING: return v.as.string && v.as.string->length > 0;
        case VAL_AGENT:  return v.as.agent != NULL;
        default:         return true;
    }
}

// ============================================================================
// Comparison
// ============================================================================

bool value_equals(Value a, Value b) {
    if (a.type != b.type) {
        // Allow int/float comparison
        if (value_is_number(a) && value_is_number(b)) {
            return value_as_number(a) == value_as_number(b);
        }
        return false;
    }

    switch (a.type) {
        case VAL_NULL:   return true;
        case VAL_BOOL:   return a.as.boolean == b.as.boolean;
        case VAL_INT:    return a.as.integer == b.as.integer;
        case VAL_FLOAT:  return a.as.floating == b.as.floating;
        case VAL_STRING: return vega_string_equals(a.as.string, b.as.string);
        case VAL_AGENT:  return a.as.agent == b.as.agent;
        default:         return false;
    }
}

int value_compare(Value a, Value b) {
    if (value_is_number(a) && value_is_number(b)) {
        double da = value_as_number(a);
        double db = value_as_number(b);
        if (da < db) return -1;
        if (da > db) return 1;
        return 0;
    }

    if (a.type == VAL_STRING && b.type == VAL_STRING) {
        return vega_string_compare(a.as.string, b.as.string);
    }

    // Incomparable types
    return 0;
}

// ============================================================================
// Arithmetic
// ============================================================================

Value value_add(Value a, Value b) {
    // Array concatenation
    if (a.type == VAL_ARRAY && b.type == VAL_ARRAY) {
        VegaArray* arr_a = a.as.array;
        VegaArray* arr_b = b.as.array;
        uint32_t total = (arr_a ? arr_a->count : 0) + (arr_b ? arr_b->count : 0);
        VegaArray* result = array_new(total > 0 ? total : 4);

        if (arr_a) {
            for (uint32_t i = 0; i < arr_a->count; i++) {
                array_push(result, arr_a->items[i]);
            }
        }
        if (arr_b) {
            for (uint32_t i = 0; i < arr_b->count; i++) {
                array_push(result, arr_b->items[i]);
            }
        }

        return (Value){.type = VAL_ARRAY, .as.array = result};
    }

    // String concatenation
    if (a.type == VAL_STRING || b.type == VAL_STRING) {
        VegaString* sa = value_to_string(a);
        VegaString* sb = value_to_string(b);
        VegaString* result = vega_string_concat(sa, sb);

        // Release temporaries if we created them
        if (a.type != VAL_STRING) vega_obj_release(sa);
        if (b.type != VAL_STRING) vega_obj_release(sb);

        return value_string(result);
    }

    // Numeric addition
    if (a.type == VAL_INT && b.type == VAL_INT) {
        return value_int(a.as.integer + b.as.integer);
    }

    if (value_is_number(a) && value_is_number(b)) {
        return value_float(value_as_number(a) + value_as_number(b));
    }

    return value_null();
}

Value value_sub(Value a, Value b) {
    if (a.type == VAL_INT && b.type == VAL_INT) {
        return value_int(a.as.integer - b.as.integer);
    }
    if (value_is_number(a) && value_is_number(b)) {
        return value_float(value_as_number(a) - value_as_number(b));
    }
    return value_null();
}

Value value_mul(Value a, Value b) {
    if (a.type == VAL_INT && b.type == VAL_INT) {
        return value_int(a.as.integer * b.as.integer);
    }
    if (value_is_number(a) && value_is_number(b)) {
        return value_float(value_as_number(a) * value_as_number(b));
    }
    return value_null();
}

Value value_div(Value a, Value b) {
    // Integer division returns integer (truncates toward zero)
    if (a.type == VAL_INT && b.type == VAL_INT) {
        if (b.as.integer == 0) return value_null();
        return value_int(a.as.integer / b.as.integer);
    }

    // Float division
    double db = value_as_number(b);
    if (db == 0.0) {
        return value_null();  // Division by zero
    }
    return value_float(value_as_number(a) / db);
}

Value value_mod(Value a, Value b) {
    if (a.type == VAL_INT && b.type == VAL_INT) {
        if (b.as.integer == 0) return value_null();
        return value_int(a.as.integer % b.as.integer);
    }

    double da = value_as_number(a);
    double db = value_as_number(b);
    if (db == 0.0) return value_null();
    return value_float(fmod(da, db));
}

Value value_neg(Value v) {
    if (v.type == VAL_INT) {
        return value_int(-v.as.integer);
    }
    if (v.type == VAL_FLOAT) {
        return value_float(-v.as.floating);
    }
    return value_null();
}

// ============================================================================
// String Conversion
// ============================================================================

VegaString* value_to_string(Value v) {
    char buffer[64];

    switch (v.type) {
        case VAL_NULL:
            return vega_string_from_cstr("null");

        case VAL_BOOL:
            return vega_string_from_cstr(v.as.boolean ? "true" : "false");

        case VAL_INT:
            snprintf(buffer, sizeof(buffer), "%lld", (long long)v.as.integer);
            return vega_string_from_cstr(buffer);

        case VAL_FLOAT:
            snprintf(buffer, sizeof(buffer), "%g", v.as.floating);
            return vega_string_from_cstr(buffer);

        case VAL_STRING:
            vega_obj_retain(v.as.string);
            return v.as.string;

        case VAL_AGENT:
            snprintf(buffer, sizeof(buffer), "<agent %p>", (void*)v.as.agent);
            return vega_string_from_cstr(buffer);

        case VAL_FUTURE:
            return vega_string_from_cstr("<future>");

        case VAL_ARRAY:
            return vega_string_from_cstr("<array>");

        case VAL_RESULT:
            return vega_string_from_cstr("<result>");

        case VAL_FUNCTION:
            snprintf(buffer, sizeof(buffer), "<function %u>", v.as.function_id);
            return vega_string_from_cstr(buffer);

        default:
            return vega_string_from_cstr("<unknown>");
    }
}

// ============================================================================
// Reference Counting
// ============================================================================

void value_retain(Value v) {
    switch (v.type) {
        case VAL_STRING:
            if (v.as.string) vega_obj_retain(v.as.string);
            break;
        case VAL_ARRAY:
            if (v.as.array) vega_obj_retain(v.as.array);
            break;
        case VAL_RESULT:
            if (v.as.result) vega_obj_retain(v.as.result);
            break;
        default:
            break;
    }
}

void value_release(Value v) {
    switch (v.type) {
        case VAL_STRING:
            if (v.as.string) vega_obj_release(v.as.string);
            break;
        case VAL_ARRAY:
            if (v.as.array) vega_obj_release(v.as.array);
            break;
        case VAL_RESULT:
            if (v.as.result) vega_obj_release(v.as.result);
            break;
        default:
            break;
    }
}

Value value_clone(Value v) {
    value_retain(v);
    return v;
}

// ============================================================================
// Debug Printing
// ============================================================================

void value_print(Value v) {
    switch (v.type) {
        case VAL_NULL:
            printf("null");
            break;
        case VAL_BOOL:
            printf("%s", v.as.boolean ? "true" : "false");
            break;
        case VAL_INT:
            printf("%lld", (long long)v.as.integer);
            break;
        case VAL_FLOAT:
            printf("%g", v.as.floating);
            break;
        case VAL_STRING:
            if (v.as.string) {
                printf("%s", v.as.string->data);
            } else {
                printf("(null string)");
            }
            break;
        case VAL_AGENT:
            printf("<agent %p>", (void*)v.as.agent);
            break;
        case VAL_FUTURE:
            printf("<future>");
            break;
        case VAL_ARRAY:
            if (v.as.array) {
                printf("[");
                for (uint32_t i = 0; i < v.as.array->count; i++) {
                    if (i > 0) printf(", ");
                    value_print(v.as.array->items[i]);
                }
                printf("]");
            } else {
                printf("[]");
            }
            break;
        case VAL_RESULT:
            printf("<result>");
            break;
        case VAL_FUNCTION:
            printf("<function %u>", v.as.function_id);
            break;
        default:
            printf("<unknown>");
            break;
    }
}

const char* value_type_name(ValueType type) {
    switch (type) {
        case VAL_NULL:     return "null";
        case VAL_BOOL:     return "bool";
        case VAL_INT:      return "int";
        case VAL_FLOAT:    return "float";
        case VAL_STRING:   return "string";
        case VAL_AGENT:    return "agent";
        case VAL_FUTURE:   return "future";
        case VAL_ARRAY:    return "array";
        case VAL_RESULT:   return "result";
        case VAL_FUNCTION: return "function";
        default:           return "unknown";
    }
}

// ============================================================================
// Array Operations
// ============================================================================

VegaArray* array_new(uint32_t initial_capacity) {
    VegaArray* arr = vega_obj_alloc(sizeof(VegaArray), OBJ_ARRAY);
    if (!arr) return NULL;

    arr->capacity = initial_capacity > 0 ? initial_capacity : 8;
    arr->count = 0;
    arr->items = malloc(arr->capacity * sizeof(Value));

    return arr;
}

void array_push(VegaArray* arr, Value v) {
    if (!arr) return;

    if (arr->count >= arr->capacity) {
        arr->capacity *= 2;
        arr->items = realloc(arr->items, arr->capacity * sizeof(Value));
    }

    value_retain(v);
    arr->items[arr->count++] = v;
}

Value array_get(VegaArray* arr, uint32_t index) {
    if (!arr || index >= arr->count) {
        return value_null();
    }
    return arr->items[index];
}

void array_set(VegaArray* arr, uint32_t index, Value v) {
    if (!arr || index >= arr->count) return;

    value_release(arr->items[index]);
    value_retain(v);
    arr->items[index] = v;
}

uint32_t array_length(VegaArray* arr) {
    return arr ? arr->count : 0;
}

// ============================================================================
// Result Operations
// ============================================================================

VegaResult* result_ok(Value value) {
    VegaResult* r = vega_obj_alloc(sizeof(VegaResult), OBJ_RESULT);
    if (!r) return NULL;

    r->is_ok = true;
    r->value = value;
    value_retain(value);

    return r;
}

VegaResult* result_err(Value error) {
    VegaResult* r = vega_obj_alloc(sizeof(VegaResult), OBJ_RESULT);
    if (!r) return NULL;

    r->is_ok = false;
    r->value = error;
    value_retain(error);

    return r;
}

bool result_is_ok(VegaResult* r) {
    return r && r->is_ok;
}

Value result_unwrap(VegaResult* r) {
    if (!r || !r->is_ok) return value_null();
    return r->value;
}

Value result_unwrap_err(VegaResult* r) {
    if (!r || r->is_ok) return value_null();
    return r->value;
}
