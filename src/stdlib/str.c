#include "../vm/value.h"
#include "../common/memory.h"
#include <string.h>
#include <stdlib.h>

/*
 * str module - String operations
 *
 * Functions:
 *   str::len(s: str) -> int
 *   str::contains(s: str, substr: str) -> bool
 *   str::concat(a: str, b: str) -> str
 *   str::substr(s: str, start: int, len: int) -> str
 *   str::split(s: str, delimiter: str) -> str[]
 */

// str::len implementation is in vm.c (call_native)
// str::contains implementation is in vm.c (call_native)

// Additional string functions

VegaString* str_concat(VegaString* a, VegaString* b) {
    return vega_string_concat(a, b);
}

VegaString* str_substr(VegaString* s, uint32_t start, uint32_t len) {
    return vega_string_substr(s, start, len);
}

// Split string by delimiter
// Returns array of strings (caller must free each string and the array)
VegaString** str_split(VegaString* s, VegaString* delim, uint32_t* out_count) {
    if (!s || !delim || delim->length == 0) {
        *out_count = 0;
        return NULL;
    }

    // Count occurrences
    uint32_t count = 1;
    const char* p = s->data;
    while ((p = strstr(p, delim->data)) != NULL) {
        count++;
        p += delim->length;
    }

    // Allocate result array
    VegaString** result = malloc(count * sizeof(VegaString*));
    if (!result) {
        *out_count = 0;
        return NULL;
    }

    // Split
    uint32_t idx = 0;
    const char* start = s->data;
    const char* end = s->data;

    while ((end = strstr(start, delim->data)) != NULL) {
        result[idx++] = vega_string_new(start, (uint32_t)(end - start));
        start = end + delim->length;
    }

    // Last segment
    result[idx++] = vega_string_from_cstr(start);

    *out_count = count;
    return result;
}
