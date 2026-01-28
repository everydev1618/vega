#include "../vm/value.h"
#include "../common/memory.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/*
 * json module - JSON parsing and generation
 *
 * Functions:
 *   json::parse(s: str) -> value
 *   json::stringify(v: value) -> str
 *   json::get(obj: value, key: str) -> value
 *
 * Note: This is a minimal JSON implementation.
 * For production use, consider integrating cJSON.
 */

// Simple JSON string extraction
// Finds a string value for a given key
VegaString* json_get_string(const char* json, const char* key) {
    if (!json || !key) return NULL;

    // Build search pattern: "key":
    size_t key_len = strlen(key);
    char* pattern = malloc(key_len + 4);
    sprintf(pattern, "\"%s\":", key);

    const char* pos = strstr(json, pattern);
    free(pattern);

    if (!pos) return NULL;

    pos += key_len + 3;  // Skip past "key":

    // Skip whitespace
    while (*pos && isspace(*pos)) pos++;

    if (*pos != '"') return NULL;
    pos++;

    // Find end of string
    const char* end = pos;
    while (*end && *end != '"') {
        if (*end == '\\' && *(end + 1)) end += 2;
        else end++;
    }

    return vega_string_new(pos, (uint32_t)(end - pos));
}

// Simple JSON number extraction
int64_t json_get_int(const char* json, const char* key, bool* found) {
    if (!json || !key) {
        if (found) *found = false;
        return 0;
    }

    size_t key_len = strlen(key);
    char* pattern = malloc(key_len + 4);
    sprintf(pattern, "\"%s\":", key);

    const char* pos = strstr(json, pattern);
    free(pattern);

    if (!pos) {
        if (found) *found = false;
        return 0;
    }

    pos += key_len + 3;
    while (*pos && isspace(*pos)) pos++;

    if (found) *found = true;
    return strtoll(pos, NULL, 10);
}

// Simple JSON boolean extraction
bool json_get_bool(const char* json, const char* key, bool* found) {
    if (!json || !key) {
        if (found) *found = false;
        return false;
    }

    size_t key_len = strlen(key);
    char* pattern = malloc(key_len + 4);
    sprintf(pattern, "\"%s\":", key);

    const char* pos = strstr(json, pattern);
    free(pattern);

    if (!pos) {
        if (found) *found = false;
        return false;
    }

    pos += key_len + 3;
    while (*pos && isspace(*pos)) pos++;

    if (found) *found = true;
    return strncmp(pos, "true", 4) == 0;
}

// Simple JSON stringify for basic values
VegaString* json_stringify_value(Value v) {
    char buffer[256];

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

        case VAL_STRING: {
            // Need to escape the string
            VegaString* s = v.as.string;
            size_t needed = s->length * 2 + 3;  // Worst case + quotes + null
            char* result = malloc(needed);
            char* p = result;

            *p++ = '"';
            for (uint32_t i = 0; i < s->length; i++) {
                char c = s->data[i];
                switch (c) {
                    case '"':  *p++ = '\\'; *p++ = '"'; break;
                    case '\\': *p++ = '\\'; *p++ = '\\'; break;
                    case '\n': *p++ = '\\'; *p++ = 'n'; break;
                    case '\r': *p++ = '\\'; *p++ = 'r'; break;
                    case '\t': *p++ = '\\'; *p++ = 't'; break;
                    default:   *p++ = c; break;
                }
            }
            *p++ = '"';
            *p = '\0';

            VegaString* str = vega_string_from_cstr(result);
            free(result);
            return str;
        }

        default:
            return vega_string_from_cstr("null");
    }
}
