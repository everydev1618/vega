#ifndef VEGA_MEMORY_H
#define VEGA_MEMORY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/*
 * Vega Memory Management
 *
 * Reference-counted memory with length-prefixed strings.
 * All heap-allocated objects have a reference count header.
 */

// ============================================================================
// Reference-Counted Object Header
// ============================================================================

typedef struct {
    uint32_t refcount;     // Reference count
    uint32_t size;         // Size of data (excluding header)
    uint8_t  type;         // Object type tag
    uint8_t  flags;        // Object flags
    uint16_t reserved;     // Padding/reserved
} VegaObjHeader;

// Object types
typedef enum {
    OBJ_STRING  = 0x01,
    OBJ_ARRAY   = 0x02,
    OBJ_AGENT   = 0x03,
    OBJ_RESULT  = 0x04,
    OBJ_MAP     = 0x05,
} VegaObjType;

// Object flags
#define OBJ_FLAG_NONE     0x00
#define OBJ_FLAG_INTERNED 0x01  // String is interned (don't free)
#define OBJ_FLAG_FREED    0x02  // Object has been freed (for debugging)

// ============================================================================
// Length-Prefixed String
// ============================================================================

// Note: VegaObjHeader is prepended by vega_obj_alloc, not part of this struct
typedef struct {
    uint32_t length;       // String length (not including null terminator)
    char data[];           // Null-terminated string data (flexible array member)
} VegaString;

// ============================================================================
// Memory Functions
// ============================================================================

// Initialize/shutdown memory system
void vega_memory_init(void);
void vega_memory_shutdown(void);

// Basic allocation (not ref-counted)
void* vega_alloc(size_t size);
void* vega_realloc(void* ptr, size_t new_size);
void  vega_free(void* ptr);

// Ref-counted object allocation
void* vega_obj_alloc(size_t size, VegaObjType type);
void  vega_obj_retain(void* obj);
void  vega_obj_release(void* obj);

// Get header from object pointer
static inline VegaObjHeader* vega_obj_header(void* obj) {
    return (VegaObjHeader*)((char*)obj - sizeof(VegaObjHeader));
}

// Get object from header
static inline void* vega_header_obj(VegaObjHeader* header) {
    return (void*)((char*)header + sizeof(VegaObjHeader));
}

// ============================================================================
// String Functions
// ============================================================================

// Create new string (copies data)
VegaString* vega_string_new(const char* data, uint32_t length);

// Create string from C string (null-terminated)
VegaString* vega_string_from_cstr(const char* cstr);

// Create empty string with given capacity
VegaString* vega_string_with_capacity(uint32_t capacity);

// String operations (return new strings, properly ref-counted)
VegaString* vega_string_concat(VegaString* a, VegaString* b);
VegaString* vega_string_substr(VegaString* str, uint32_t start, uint32_t len);

// String queries
bool vega_string_contains(VegaString* str, VegaString* substr);
bool vega_string_equals(VegaString* a, VegaString* b);
int  vega_string_compare(VegaString* a, VegaString* b);

// Get C string pointer (valid while string is alive)
static inline const char* vega_string_cstr(VegaString* str) {
    return str->data;
}

// Get string length
static inline uint32_t vega_string_length(VegaString* str) {
    return str->length;
}

// ============================================================================
// Retain/Release Macros
// ============================================================================

#define RETAIN(obj)  do { if (obj) vega_obj_retain(obj); } while(0)
#define RELEASE(obj) do { if (obj) vega_obj_release(obj); } while(0)

// ============================================================================
// Memory Statistics (for debugging)
// ============================================================================

typedef struct {
    size_t total_allocated;
    size_t total_freed;
    size_t current_usage;
    size_t peak_usage;
    size_t allocation_count;
    size_t free_count;
    size_t object_count;     // Currently live ref-counted objects
} VegaMemoryStats;

void vega_memory_get_stats(VegaMemoryStats* stats);
void vega_memory_print_stats(void);

// Debug: check for leaks (returns number of leaked objects)
size_t vega_memory_check_leaks(void);

#endif // VEGA_MEMORY_H
