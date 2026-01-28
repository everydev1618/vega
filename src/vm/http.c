#include "http.h"
#include <curl/curl.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// ============================================================================
// CURL Helpers
// ============================================================================

typedef struct {
    char* data;
    size_t size;
} ResponseBuffer;

static size_t write_callback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t realsize = size * nmemb;
    ResponseBuffer* buf = (ResponseBuffer*)userp;

    char* ptr = realloc(buf->data, buf->size + realsize + 1);
    if (!ptr) return 0;

    buf->data = ptr;
    memcpy(&(buf->data[buf->size]), contents, realsize);
    buf->size += realsize;
    buf->data[buf->size] = 0;

    return realsize;
}

// ============================================================================
// Initialization
// ============================================================================

bool http_init(void) {
    CURLcode res = curl_global_init(CURL_GLOBAL_DEFAULT);
    return res == CURLE_OK;
}

void http_cleanup(void) {
    curl_global_cleanup();
}

// ============================================================================
// JSON Helpers (simple, no external dependency)
// ============================================================================

static char* json_escape_string(const char* str) {
    if (!str) return strdup("");

    size_t len = strlen(str);
    size_t escaped_len = len * 2 + 1;  // Worst case
    char* escaped = malloc(escaped_len);
    if (!escaped) return NULL;

    char* p = escaped;
    for (size_t i = 0; i < len; i++) {
        char c = str[i];
        switch (c) {
            case '"':  *p++ = '\\'; *p++ = '"'; break;
            case '\\': *p++ = '\\'; *p++ = '\\'; break;
            case '\n': *p++ = '\\'; *p++ = 'n'; break;
            case '\r': *p++ = '\\'; *p++ = 'r'; break;
            case '\t': *p++ = '\\'; *p++ = 't'; break;
            default:   *p++ = c; break;
        }
    }
    *p = '\0';

    return escaped;
}

// ============================================================================
// Anthropic API
// ============================================================================

HttpResponse* anthropic_send_message(
    const char* api_key,
    const char* model,
    const char* system_prompt,
    const char* user_message,
    double temperature
) {
    const char* messages[1] = { user_message };
    return anthropic_send_messages(api_key, model, system_prompt, messages, 1, temperature);
}

HttpResponse* anthropic_send_messages(
    const char* api_key,
    const char* model,
    const char* system_prompt,
    const char** messages,
    int message_count,
    double temperature
) {
    HttpResponse* resp = calloc(1, sizeof(HttpResponse));
    if (!resp) return NULL;

    CURL* curl = curl_easy_init();
    if (!curl) {
        resp->error = strdup("Failed to initialize CURL");
        return resp;
    }

    // Build JSON body
    size_t body_capacity = 4096;
    char* body = malloc(body_capacity);
    if (!body) {
        resp->error = strdup("Out of memory");
        curl_easy_cleanup(curl);
        return resp;
    }

    char* escaped_model = json_escape_string(model ? model : "claude-sonnet-4-20250514");
    char* escaped_system = json_escape_string(system_prompt ? system_prompt : "You are a helpful assistant.");

    int offset = snprintf(body, body_capacity,
        "{"
        "\"model\": \"%s\","
        "\"max_tokens\": 4096,"
        "\"temperature\": %.2f,"
        "\"system\": \"%s\","
        "\"messages\": [",
        escaped_model,
        temperature,
        escaped_system
    );

    free(escaped_model);
    free(escaped_system);

    // Add messages
    for (int i = 0; i < message_count; i++) {
        const char* role = (i % 2 == 0) ? "user" : "assistant";
        char* escaped_content = json_escape_string(messages[i]);

        size_t needed = strlen(escaped_content) + 100;
        if (offset + needed >= body_capacity) {
            body_capacity = body_capacity * 2 + needed;
            body = realloc(body, body_capacity);
        }

        offset += snprintf(body + offset, body_capacity - offset,
            "%s{\"role\": \"%s\", \"content\": \"%s\"}",
            i > 0 ? "," : "",
            role,
            escaped_content
        );

        free(escaped_content);
    }

    // Close JSON
    snprintf(body + offset, body_capacity - offset, "]}");

    // Set up CURL
    ResponseBuffer response_buf = {0};

    struct curl_slist* headers = NULL;
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", api_key ? api_key : "");
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "content-type: application/json");
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    // Perform request
    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        resp->error = strdup(curl_easy_strerror(res));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->status_code);
        resp->body = response_buf.data;
        resp->body_len = response_buf.size;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    return resp;
}

void http_response_free(HttpResponse* resp) {
    if (!resp) return;
    free(resp->body);
    free(resp->error);
    free(resp);
}

// ============================================================================
// General HTTP GET
// ============================================================================

HttpResponse* http_get(const char* url) {
    HttpResponse* resp = calloc(1, sizeof(HttpResponse));
    if (!resp) return NULL;

    CURL* curl = curl_easy_init();
    if (!curl) {
        resp->error = strdup("Failed to initialize CURL");
        return resp;
    }

    ResponseBuffer response_buf = {0};

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        resp->error = strdup(curl_easy_strerror(res));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->status_code);
        resp->body = response_buf.data;
        resp->body_len = response_buf.size;
    }

    curl_easy_cleanup(curl);
    return resp;
}

// ============================================================================
// JSON Parsing (minimal, just for extracting text)
// ============================================================================

char* anthropic_extract_text(const char* json_response) {
    if (!json_response) return NULL;

    // Look for "text": "..." in the response
    // This is a simple parser - in production you'd use a JSON library

    const char* text_key = "\"text\":";
    const char* pos = strstr(json_response, text_key);
    if (!pos) {
        // Try looking for error message
        const char* error_key = "\"message\":";
        pos = strstr(json_response, error_key);
        if (pos) {
            pos += strlen(error_key);
            while (*pos == ' ' || *pos == '\t') pos++;
            if (*pos == '"') {
                pos++;
                const char* end = pos;
                while (*end && *end != '"') {
                    if (*end == '\\' && *(end + 1)) end += 2;
                    else end++;
                }
                size_t len = end - pos;
                char* result = malloc(len + 20);
                snprintf(result, len + 20, "API Error: %.*s", (int)len, pos);
                return result;
            }
        }
        return strdup("Failed to parse response");
    }

    pos += strlen(text_key);

    // Skip whitespace
    while (*pos == ' ' || *pos == '\t' || *pos == '\n') pos++;

    if (*pos != '"') {
        return strdup("Invalid response format");
    }
    pos++;

    // Find end of string
    const char* start = pos;
    while (*pos && *pos != '"') {
        if (*pos == '\\' && *(pos + 1)) {
            pos += 2;
        } else {
            pos++;
        }
    }

    size_t len = pos - start;
    char* result = malloc(len + 1);
    if (!result) return NULL;

    // Unescape the string
    char* dest = result;
    const char* src = start;
    while (src < pos) {
        if (*src == '\\' && src + 1 < pos) {
            src++;
            switch (*src) {
                case 'n': *dest++ = '\n'; break;
                case 'r': *dest++ = '\r'; break;
                case 't': *dest++ = '\t'; break;
                case '"': *dest++ = '"'; break;
                case '\\': *dest++ = '\\'; break;
                default: *dest++ = *src; break;
            }
            src++;
        } else {
            *dest++ = *src++;
        }
    }
    *dest = '\0';

    return result;
}

bool anthropic_has_tool_use(const char* json_response) {
    if (!json_response) return false;
    return strstr(json_response, "\"type\": \"tool_use\"") != NULL ||
           strstr(json_response, "\"type\":\"tool_use\"") != NULL;
}

char* anthropic_extract_tool_use(const char* json_response, char** tool_id, char** input_json) {
    if (!json_response) return NULL;

    // Find tool_use block
    const char* tool_use = strstr(json_response, "\"type\": \"tool_use\"");
    if (!tool_use) tool_use = strstr(json_response, "\"type\":\"tool_use\"");
    if (!tool_use) return NULL;

    // Find the containing object by going back to find {
    const char* obj_start = tool_use;
    int brace_count = 0;
    while (obj_start > json_response) {
        obj_start--;
        if (*obj_start == '}') brace_count++;
        if (*obj_start == '{') {
            if (brace_count == 0) break;
            brace_count--;
        }
    }

    // Extract "id" field
    const char* id_key = strstr(obj_start, "\"id\":");
    if (id_key && tool_id) {
        id_key += 5;
        while (*id_key == ' ' || *id_key == '\t') id_key++;
        if (*id_key == '"') {
            id_key++;
            const char* id_end = id_key;
            while (*id_end && *id_end != '"') id_end++;
            *tool_id = strndup(id_key, id_end - id_key);
        }
    }

    // Extract "name" field
    char* tool_name = NULL;
    const char* name_key = strstr(obj_start, "\"name\":");
    if (name_key) {
        name_key += 7;
        while (*name_key == ' ' || *name_key == '\t') name_key++;
        if (*name_key == '"') {
            name_key++;
            const char* name_end = name_key;
            while (*name_end && *name_end != '"') name_end++;
            tool_name = strndup(name_key, name_end - name_key);
        }
    }

    // Extract "input" field (as raw JSON)
    const char* input_key = strstr(obj_start, "\"input\":");
    if (input_key && input_json) {
        input_key += 8;
        while (*input_key == ' ' || *input_key == '\t' || *input_key == '\n') input_key++;
        if (*input_key == '{') {
            // Find matching }
            const char* input_end = input_key + 1;
            int depth = 1;
            while (*input_end && depth > 0) {
                if (*input_end == '{') depth++;
                else if (*input_end == '}') depth--;
                else if (*input_end == '"') {
                    // Skip string
                    input_end++;
                    while (*input_end && *input_end != '"') {
                        if (*input_end == '\\' && *(input_end + 1)) input_end++;
                        input_end++;
                    }
                }
                input_end++;
            }
            *input_json = strndup(input_key, input_end - input_key);
        }
    }

    return tool_name;
}

HttpResponse* anthropic_send_with_tools(
    const char* api_key,
    const char* model,
    const char* system_prompt,
    const char** messages,
    int message_count,
    ToolDefinition* tools,
    int tool_count,
    double temperature
) {
    HttpResponse* resp = calloc(1, sizeof(HttpResponse));
    if (!resp) return NULL;

    CURL* curl = curl_easy_init();
    if (!curl) {
        resp->error = strdup("Failed to initialize CURL");
        return resp;
    }

    // Build JSON body
    size_t body_capacity = 8192;
    char* body = malloc(body_capacity);
    if (!body) {
        resp->error = strdup("Out of memory");
        curl_easy_cleanup(curl);
        return resp;
    }

    char* escaped_model = json_escape_string(model ? model : "claude-sonnet-4-20250514");
    char* escaped_system = json_escape_string(system_prompt ? system_prompt : "You are a helpful assistant.");

    int offset = snprintf(body, body_capacity,
        "{"
        "\"model\": \"%s\","
        "\"max_tokens\": 4096,"
        "\"temperature\": %.2f,"
        "\"system\": \"%s\","
        "\"messages\": [",
        escaped_model,
        temperature,
        escaped_system
    );

    free(escaped_model);
    free(escaped_system);

    // Add messages
    for (int i = 0; i < message_count; i++) {
        const char* role = (i % 2 == 0) ? "user" : "assistant";
        char* escaped_content = json_escape_string(messages[i]);

        size_t needed = strlen(escaped_content) + 100;
        if (offset + needed >= body_capacity) {
            body_capacity = body_capacity * 2 + needed;
            body = realloc(body, body_capacity);
        }

        offset += snprintf(body + offset, body_capacity - offset,
            "%s{\"role\": \"%s\", \"content\": \"%s\"}",
            i > 0 ? "," : "",
            role,
            escaped_content
        );

        free(escaped_content);
    }

    offset += snprintf(body + offset, body_capacity - offset, "]");

    // Add tools if provided
    if (tool_count > 0 && tools) {
        offset += snprintf(body + offset, body_capacity - offset, ",\"tools\": [");

        for (int t = 0; t < tool_count; t++) {
            if (offset + 1024 >= (int)body_capacity) {
                body_capacity *= 2;
                body = realloc(body, body_capacity);
            }

            char* escaped_name = json_escape_string(tools[t].name);
            char* escaped_desc = json_escape_string(tools[t].description ? tools[t].description : "");

            offset += snprintf(body + offset, body_capacity - offset,
                "%s{\"name\": \"%s\", \"description\": \"%s\", \"input_schema\": {\"type\": \"object\", \"properties\": {",
                t > 0 ? "," : "",
                escaped_name,
                escaped_desc
            );

            free(escaped_name);
            free(escaped_desc);

            // Add parameters
            for (int p = 0; p < tools[t].param_count; p++) {
                const char* ptype = "string";  // Default to string
                if (tools[t].param_types && tools[t].param_types[p]) {
                    if (strcmp(tools[t].param_types[p], "int") == 0) ptype = "integer";
                    else if (strcmp(tools[t].param_types[p], "bool") == 0) ptype = "boolean";
                    else if (strcmp(tools[t].param_types[p], "float") == 0) ptype = "number";
                }

                offset += snprintf(body + offset, body_capacity - offset,
                    "%s\"%s\": {\"type\": \"%s\"}",
                    p > 0 ? "," : "",
                    tools[t].param_names[p],
                    ptype
                );
            }

            // Add required array
            offset += snprintf(body + offset, body_capacity - offset, "}, \"required\": [");
            for (int p = 0; p < tools[t].param_count; p++) {
                offset += snprintf(body + offset, body_capacity - offset,
                    "%s\"%s\"",
                    p > 0 ? "," : "",
                    tools[t].param_names[p]
                );
            }
            offset += snprintf(body + offset, body_capacity - offset, "]}}");
        }

        offset += snprintf(body + offset, body_capacity - offset, "]");
    }

    // Close JSON
    snprintf(body + offset, body_capacity - offset, "}");

    // Set up CURL
    ResponseBuffer response_buf = {0};

    struct curl_slist* headers = NULL;
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", api_key ? api_key : "");
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "content-type: application/json");
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        resp->error = strdup(curl_easy_strerror(res));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->status_code);
        resp->body = response_buf.data;
        resp->body_len = response_buf.size;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    return resp;
}

HttpResponse* anthropic_send_tool_result(
    const char* api_key,
    const char* model,
    const char* system_prompt,
    const char** messages,
    int message_count,
    const char* tool_use_id,
    const char* tool_result,
    ToolDefinition* tools,
    int tool_count,
    double temperature
) {
    HttpResponse* resp = calloc(1, sizeof(HttpResponse));
    if (!resp) return NULL;

    CURL* curl = curl_easy_init();
    if (!curl) {
        resp->error = strdup("Failed to initialize CURL");
        return resp;
    }

    size_t body_capacity = 8192;
    char* body = malloc(body_capacity);

    char* escaped_model = json_escape_string(model ? model : "claude-sonnet-4-20250514");
    char* escaped_system = json_escape_string(system_prompt ? system_prompt : "You are a helpful assistant.");

    int offset = snprintf(body, body_capacity,
        "{"
        "\"model\": \"%s\","
        "\"max_tokens\": 4096,"
        "\"temperature\": %.2f,"
        "\"system\": \"%s\","
        "\"messages\": [",
        escaped_model,
        temperature,
        escaped_system
    );

    free(escaped_model);
    free(escaped_system);

    // Add previous messages
    for (int i = 0; i < message_count; i++) {
        const char* role = (i % 2 == 0) ? "user" : "assistant";
        char* escaped_content = json_escape_string(messages[i]);

        size_t needed = strlen(escaped_content) + 100;
        if (offset + needed >= body_capacity) {
            body_capacity = body_capacity * 2 + needed;
            body = realloc(body, body_capacity);
        }

        offset += snprintf(body + offset, body_capacity - offset,
            "%s{\"role\": \"%s\", \"content\": \"%s\"}",
            i > 0 ? "," : "",
            role,
            escaped_content
        );

        free(escaped_content);
    }

    // Add tool result message
    char* escaped_result = json_escape_string(tool_result);
    size_t needed = strlen(escaped_result) + strlen(tool_use_id) + 200;
    if (offset + needed >= body_capacity) {
        body_capacity = body_capacity * 2 + needed;
        body = realloc(body, body_capacity);
    }

    offset += snprintf(body + offset, body_capacity - offset,
        ",{\"role\": \"user\", \"content\": [{\"type\": \"tool_result\", \"tool_use_id\": \"%s\", \"content\": \"%s\"}]}",
        tool_use_id,
        escaped_result
    );
    free(escaped_result);

    offset += snprintf(body + offset, body_capacity - offset, "]");

    // Add tools
    if (tool_count > 0 && tools) {
        offset += snprintf(body + offset, body_capacity - offset, ",\"tools\": [");
        for (int t = 0; t < tool_count; t++) {
            if (offset + 1024 >= (int)body_capacity) {
                body_capacity *= 2;
                body = realloc(body, body_capacity);
            }

            char* escaped_name = json_escape_string(tools[t].name);
            char* escaped_desc = json_escape_string(tools[t].description ? tools[t].description : "");

            offset += snprintf(body + offset, body_capacity - offset,
                "%s{\"name\": \"%s\", \"description\": \"%s\", \"input_schema\": {\"type\": \"object\", \"properties\": {",
                t > 0 ? "," : "",
                escaped_name,
                escaped_desc
            );

            free(escaped_name);
            free(escaped_desc);

            for (int p = 0; p < tools[t].param_count; p++) {
                const char* ptype = "string";
                if (tools[t].param_types && tools[t].param_types[p]) {
                    if (strcmp(tools[t].param_types[p], "int") == 0) ptype = "integer";
                    else if (strcmp(tools[t].param_types[p], "bool") == 0) ptype = "boolean";
                    else if (strcmp(tools[t].param_types[p], "float") == 0) ptype = "number";
                }

                offset += snprintf(body + offset, body_capacity - offset,
                    "%s\"%s\": {\"type\": \"%s\"}",
                    p > 0 ? "," : "",
                    tools[t].param_names[p],
                    ptype
                );
            }

            offset += snprintf(body + offset, body_capacity - offset, "}, \"required\": [");
            for (int p = 0; p < tools[t].param_count; p++) {
                offset += snprintf(body + offset, body_capacity - offset,
                    "%s\"%s\"",
                    p > 0 ? "," : "",
                    tools[t].param_names[p]
                );
            }
            offset += snprintf(body + offset, body_capacity - offset, "]}}");
        }
        offset += snprintf(body + offset, body_capacity - offset, "]");
    }

    snprintf(body + offset, body_capacity - offset, "}");

    // Set up CURL
    ResponseBuffer response_buf = {0};

    struct curl_slist* headers = NULL;
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", api_key ? api_key : "");
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "content-type: application/json");
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        resp->error = strdup(curl_easy_strerror(res));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->status_code);
        resp->body = response_buf.data;
        resp->body_len = response_buf.size;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    return resp;
}

HttpResponse* anthropic_send_tool_result_v2(
    const char* api_key,
    const char* model,
    const char* system_prompt,
    const char** messages,
    int message_count,
    const char* assistant_content,
    const char* tool_use_id,
    const char* tool_result,
    ToolDefinition* tools,
    int tool_count,
    double temperature
) {
    HttpResponse* resp = calloc(1, sizeof(HttpResponse));
    if (!resp) return NULL;

    CURL* curl = curl_easy_init();
    if (!curl) {
        resp->error = strdup("Failed to initialize CURL");
        return resp;
    }

    size_t body_capacity = 16384;
    char* body = malloc(body_capacity);

    char* escaped_model = json_escape_string(model ? model : "claude-sonnet-4-20250514");
    char* escaped_system = json_escape_string(system_prompt ? system_prompt : "You are a helpful assistant.");

    int offset = snprintf(body, body_capacity,
        "{\"model\": \"%s\",\"max_tokens\": 4096,\"temperature\": %.2f,\"system\": \"%s\",\"messages\": [",
        escaped_model, temperature, escaped_system
    );

    free(escaped_model);
    free(escaped_system);

    for (int i = 0; i < message_count; i++) {
        const char* role = (i % 2 == 0) ? "user" : "assistant";
        char* escaped_content = json_escape_string(messages[i]);
        size_t needed = strlen(escaped_content) + 100;
        if (offset + needed >= body_capacity) {
            body_capacity = body_capacity * 2 + needed;
            body = realloc(body, body_capacity);
        }
        offset += snprintf(body + offset, body_capacity - offset,
            "%s{\"role\": \"%s\", \"content\": \"%s\"}",
            i > 0 ? "," : "", role, escaped_content);
        free(escaped_content);
    }

    if (assistant_content) {
        size_t needed = strlen(assistant_content) + 100;
        if (offset + needed >= body_capacity) {
            body_capacity = body_capacity * 2 + needed;
            body = realloc(body, body_capacity);
        }
        offset += snprintf(body + offset, body_capacity - offset,
            ",{\"role\": \"assistant\", \"content\": %s}", assistant_content);
    }

    char* escaped_result = json_escape_string(tool_result);
    size_t needed = strlen(escaped_result) + strlen(tool_use_id) + 200;
    if (offset + needed >= body_capacity) {
        body_capacity = body_capacity * 2 + needed;
        body = realloc(body, body_capacity);
    }
    offset += snprintf(body + offset, body_capacity - offset,
        ",{\"role\": \"user\", \"content\": [{\"type\": \"tool_result\", \"tool_use_id\": \"%s\", \"content\": \"%s\"}]}]",
        tool_use_id, escaped_result);
    free(escaped_result);

    if (tool_count > 0 && tools) {
        offset += snprintf(body + offset, body_capacity - offset, ",\"tools\": [");
        for (int t = 0; t < tool_count; t++) {
            if (offset + 1024 >= (int)body_capacity) {
                body_capacity *= 2;
                body = realloc(body, body_capacity);
            }
            char* escaped_name = json_escape_string(tools[t].name);
            char* escaped_desc = json_escape_string(tools[t].description ? tools[t].description : "");
            offset += snprintf(body + offset, body_capacity - offset,
                "%s{\"name\": \"%s\", \"description\": \"%s\", \"input_schema\": {\"type\": \"object\", \"properties\": {",
                t > 0 ? "," : "", escaped_name, escaped_desc);
            free(escaped_name);
            free(escaped_desc);

            for (int p = 0; p < tools[t].param_count; p++) {
                const char* ptype = "string";
                if (tools[t].param_types && tools[t].param_types[p]) {
                    if (strcmp(tools[t].param_types[p], "int") == 0) ptype = "integer";
                    else if (strcmp(tools[t].param_types[p], "bool") == 0) ptype = "boolean";
                    else if (strcmp(tools[t].param_types[p], "float") == 0) ptype = "number";
                }
                offset += snprintf(body + offset, body_capacity - offset,
                    "%s\"%s\": {\"type\": \"%s\"}", p > 0 ? "," : "", tools[t].param_names[p], ptype);
            }

            offset += snprintf(body + offset, body_capacity - offset, "}, \"required\": [");
            for (int p = 0; p < tools[t].param_count; p++) {
                offset += snprintf(body + offset, body_capacity - offset,
                    "%s\"%s\"", p > 0 ? "," : "", tools[t].param_names[p]);
            }
            offset += snprintf(body + offset, body_capacity - offset, "]}}");
        }
        offset += snprintf(body + offset, body_capacity - offset, "]");
    }

    snprintf(body + offset, body_capacity - offset, "}");

    ResponseBuffer response_buf = {0};
    struct curl_slist* headers = NULL;
    char auth_header[256];
    snprintf(auth_header, sizeof(auth_header), "x-api-key: %s", api_key ? api_key : "");
    headers = curl_slist_append(headers, auth_header);
    headers = curl_slist_append(headers, "content-type: application/json");
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
        resp->error = strdup(curl_easy_strerror(res));
    } else {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &resp->status_code);
        resp->body = response_buf.data;
        resp->body_len = response_buf.size;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(body);

    return resp;
}
