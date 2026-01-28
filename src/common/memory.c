#include "memory.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// Memory Statistics
// ============================================================================

static VegaMemoryStats g_stats = {0};

// ============================================================================
// Basic Allocation
// ============================================================================

void vega_memory_init(void) {
    memset(&g_stats, 0, sizeof(g_stats));
}

void vega_memory_shutdown(void) {
    // Could print leak warnings here
    if (g_stats.object_count > 0) {
        fprintf(stderr, "Warning: %zu objects still allocated at shutdown\n",
                g_stats.object_count);
    }
}

void* vega_alloc(size_t size) {
    void* ptr = malloc(size);
    if (ptr) {
        g_stats.total_allocated += size;
        g_stats.current_usage += size;
        g_stats.allocation_count++;
        if (g_stats.current_usage > g_stats.peak_usage) {
            g_stats.peak_usage = g_stats.current_usage;
        }
    }
    return ptr;
}

void* vega_realloc(void* ptr, size_t new_size) {
    // Note: we lose track of the old size here, stats become approximate
    void* new_ptr = realloc(ptr, new_size);
    return new_ptr;
}

void vega_free(void* ptr) {
    if (ptr) {
        g_stats.free_count++;
        free(ptr);
    }
}

// ============================================================================
// Reference-Counted Objects
// ============================================================================

void* vega_obj_alloc(size_t size, VegaObjType type) {
    // Allocate header + object data
    VegaObjHeader* header = (VegaObjHeader*)malloc(sizeof(VegaObjHeader) + size);
    if (!header) {
        return NULL;
    }

    header->refcount = 1;
    header->size = (uint32_t)size;
    header->type = (uint8_t)type;
    header->flags = OBJ_FLAG_NONE;
    header->reserved = 0;

    g_stats.total_allocated += sizeof(VegaObjHeader) + size;
    g_stats.current_usage += sizeof(VegaObjHeader) + size;
    g_stats.allocation_count++;
    g_stats.object_count++;

    if (g_stats.current_usage > g_stats.peak_usage) {
        g_stats.peak_usage = g_stats.current_usage;
    }

    return vega_header_obj(header);
}

void vega_obj_retain(void* obj) {
    if (!obj) return;
    VegaObjHeader* header = vega_obj_header(obj);

    // Check for use-after-free
    if (header->flags & OBJ_FLAG_FREED) {
        fprintf(stderr, "Error: retain on freed object!\n");
        return;
    }

    header->refcount++;
}

void vega_obj_release(void* obj) {
    if (!obj) return;
    VegaObjHeader* header = vega_obj_header(obj);

    // Check for use-after-free
    if (header->flags & OBJ_FLAG_FREED) {
        fprintf(stderr, "Error: release on freed object!\n");
        return;
    }

    // Don't free interned strings
    if (header->flags & OBJ_FLAG_INTERNED) {
        return;
    }

    header->refcount--;

    if (header->refcount == 0) {
        size_t total_size = sizeof(VegaObjHeader) + header->size;

        // Mark as freed for debugging
        header->flags |= OBJ_FLAG_FREED;

        g_stats.total_freed += total_size;
        g_stats.current_usage -= total_size;
        g_stats.free_count++;
        g_stats.object_count--;

        free(header);
    }
}

// ============================================================================
// String Functions
// ============================================================================

VegaString* vega_string_new(const char* data, uint32_t length) {
    // Allocate string object: header is handled by vega_obj_alloc
    // We need: VegaString fields + data + null terminator
    size_t str_size = sizeof(VegaString) + length + 1;

    VegaString* str = (VegaString*)vega_obj_alloc(str_size, OBJ_STRING);
    if (!str) {
        return NULL;
    }

    str->length = length;
    if (data) {
        memcpy(str->data, data, length);
    }
    str->data[length] = '\0';

    return str;
}

VegaString* vega_string_from_cstr(const char* cstr) {
    if (!cstr) {
        return vega_string_new(NULL, 0);
    }
    return vega_string_new(cstr, (uint32_t)strlen(cstr));
}

VegaString* vega_string_with_capacity(uint32_t capacity) {
    size_t str_size = sizeof(VegaString) + capacity + 1;

    VegaString* str = (VegaString*)vega_obj_alloc(str_size, OBJ_STRING);
    if (!str) {
        return NULL;
    }

    str->length = 0;
    str->data[0] = '\0';

    return str;
}

VegaString* vega_string_concat(VegaString* a, VegaString* b) {
    if (!a) return b ? (vega_obj_retain(b), b) : NULL;
    if (!b) return (vega_obj_retain(a), a);

    uint32_t new_length = a->length + b->length;
    VegaString* result = vega_string_new(NULL, new_length);
    if (!result) {
        return NULL;
    }

    memcpy(result->data, a->data, a->length);
    memcpy(result->data + a->length, b->data, b->length);
    result->data[new_length] = '\0';
    result->length = new_length;

    return result;
}

VegaString* vega_string_substr(VegaString* str, uint32_t start, uint32_t len) {
    if (!str || start >= str->length) {
        return vega_string_new(NULL, 0);
    }

    // Clamp length
    if (start + len > str->length) {
        len = str->length - start;
    }

    return vega_string_new(str->data + start, len);
}

bool vega_string_contains(VegaString* str, VegaString* substr) {
    if (!str || !substr) return false;
    if (substr->length == 0) return true;
    if (substr->length > str->length) return false;

    return strstr(str->data, substr->data) != NULL;
}

bool vega_string_equals(VegaString* a, VegaString* b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->length != b->length) return false;
    return memcmp(a->data, b->data, a->length) == 0;
}

int vega_string_compare(VegaString* a, VegaString* b) {
    if (a == b) return 0;
    if (!a) return -1;
    if (!b) return 1;
    return strcmp(a->data, b->data);
}

// ============================================================================
// Memory Statistics
// ============================================================================

void vega_memory_get_stats(VegaMemoryStats* stats) {
    if (stats) {
        *stats = g_stats;
    }
}

void vega_memory_print_stats(void) {
    printf("=== Vega Memory Stats ===\n");
    printf("Total allocated:  %zu bytes\n", g_stats.total_allocated);
    printf("Total freed:      %zu bytes\n", g_stats.total_freed);
    printf("Current usage:    %zu bytes\n", g_stats.current_usage);
    printf("Peak usage:       %zu bytes\n", g_stats.peak_usage);
    printf("Allocation count: %zu\n", g_stats.allocation_count);
    printf("Free count:       %zu\n", g_stats.free_count);
    printf("Live objects:     %zu\n", g_stats.object_count);
    printf("=========================\n");
}

size_t vega_memory_check_leaks(void) {
    return g_stats.object_count;
}
