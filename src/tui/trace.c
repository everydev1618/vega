#include "trace.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/time.h>
#include <pthread.h>

// ============================================================================
// Global State
// ============================================================================

static struct {
    bool initialized;
    bool enabled;
    pthread_mutex_t mutex;
    struct {
        TraceCallback callback;
        void* userdata;
        bool active;
    } subscribers[TRACE_MAX_SUBSCRIBERS];
    int subscriber_count;
} g_trace = {0};

// ============================================================================
// Initialization
// ============================================================================

void trace_init(void) {
    if (g_trace.initialized) return;

    memset(&g_trace, 0, sizeof(g_trace));
    pthread_mutex_init(&g_trace.mutex, NULL);
    g_trace.initialized = true;
    g_trace.enabled = true;  // Enabled by default when initialized
}

void trace_shutdown(void) {
    if (!g_trace.initialized) return;

    pthread_mutex_lock(&g_trace.mutex);
    // Clear all subscribers
    for (int i = 0; i < TRACE_MAX_SUBSCRIBERS; i++) {
        g_trace.subscribers[i].active = false;
        g_trace.subscribers[i].callback = NULL;
        g_trace.subscribers[i].userdata = NULL;
    }
    g_trace.subscriber_count = 0;
    g_trace.initialized = false;
    g_trace.enabled = false;
    pthread_mutex_unlock(&g_trace.mutex);
    pthread_mutex_destroy(&g_trace.mutex);
}

bool trace_is_enabled(void) {
    return g_trace.initialized && g_trace.enabled;
}

void trace_set_enabled(bool enabled) {
    g_trace.enabled = enabled;
}

// ============================================================================
// Subscription
// ============================================================================

int trace_subscribe(TraceCallback callback, void* userdata) {
    if (!g_trace.initialized || !callback) return 0;

    pthread_mutex_lock(&g_trace.mutex);

    // Find empty slot
    int result = 0;
    for (int i = 0; i < TRACE_MAX_SUBSCRIBERS; i++) {
        if (!g_trace.subscribers[i].active) {
            g_trace.subscribers[i].callback = callback;
            g_trace.subscribers[i].userdata = userdata;
            g_trace.subscribers[i].active = true;
            g_trace.subscriber_count++;
            result = i + 1;  // Return 1-based ID
            break;
        }
    }

    pthread_mutex_unlock(&g_trace.mutex);
    return result;
}

void trace_unsubscribe(int subscriber_id) {
    if (!g_trace.initialized) return;
    if (subscriber_id < 1 || subscriber_id > TRACE_MAX_SUBSCRIBERS) return;

    pthread_mutex_lock(&g_trace.mutex);

    int idx = subscriber_id - 1;
    if (g_trace.subscribers[idx].active) {
        g_trace.subscribers[idx].active = false;
        g_trace.subscribers[idx].callback = NULL;
        g_trace.subscribers[idx].userdata = NULL;
        g_trace.subscriber_count--;
    }

    pthread_mutex_unlock(&g_trace.mutex);
}

// ============================================================================
// Event Emission
// ============================================================================

void trace_emit(TraceEvent* event) {
    if (!g_trace.initialized || !g_trace.enabled || !event) return;
    if (g_trace.subscriber_count == 0) return;

    pthread_mutex_lock(&g_trace.mutex);

    // Set timestamp if not already set
    if (event->timestamp_ms == 0) {
        event->timestamp_ms = trace_get_time_ms();
    }

    // Notify all subscribers
    for (int i = 0; i < TRACE_MAX_SUBSCRIBERS; i++) {
        if (g_trace.subscribers[i].active && g_trace.subscribers[i].callback) {
            g_trace.subscribers[i].callback(event, g_trace.subscribers[i].userdata);
        }
    }

    pthread_mutex_unlock(&g_trace.mutex);
}

// ============================================================================
// Convenience Emitters
// ============================================================================

void trace_agent_spawn(uint32_t agent_id, const char* name, const char* model) {
    if (!trace_is_enabled()) return;

    // Build data string
    char data[512];
    snprintf(data, sizeof(data), "{\"model\":\"%s\"}", model ? model : "unknown");

    TraceEvent event = {
        .type = TRACE_AGENT_SPAWN,
        .timestamp_ms = trace_get_time_ms(),
        .agent_id = agent_id,
        .agent_name = name ? strdup(name) : NULL,
        .data = strdup(data),
        .is_error = false
    };

    trace_emit(&event);

    free(event.agent_name);
    free(event.data);
}

void trace_agent_free(uint32_t agent_id, const char* name) {
    if (!trace_is_enabled()) return;

    TraceEvent event = {
        .type = TRACE_AGENT_FREE,
        .timestamp_ms = trace_get_time_ms(),
        .agent_id = agent_id,
        .agent_name = name ? strdup(name) : NULL,
        .is_error = false
    };

    trace_emit(&event);

    free(event.agent_name);
}

void trace_msg_send(uint32_t agent_id, const char* agent_name, const char* message) {
    if (!trace_is_enabled()) return;

    TraceEvent event = {
        .type = TRACE_MSG_SEND,
        .timestamp_ms = trace_get_time_ms(),
        .agent_id = agent_id,
        .agent_name = agent_name ? strdup(agent_name) : NULL,
        .data = message ? strdup(message) : NULL,
        .is_error = false
    };

    trace_emit(&event);

    free(event.agent_name);
    free(event.data);
}

void trace_msg_recv(uint32_t agent_id, const char* agent_name, const char* response,
                    TokenUsage* tokens, uint64_t duration_ms) {
    if (!trace_is_enabled()) return;

    TraceEvent event = {
        .type = TRACE_MSG_RECV,
        .timestamp_ms = trace_get_time_ms(),
        .agent_id = agent_id,
        .agent_name = agent_name ? strdup(agent_name) : NULL,
        .data = response ? strdup(response) : NULL,
        .duration_ms = duration_ms,
        .is_error = false
    };

    if (tokens) {
        event.tokens = *tokens;
    }

    trace_emit(&event);

    free(event.agent_name);
    free(event.data);
}

void trace_tool_call(uint32_t agent_id, const char* agent_name,
                     const char* tool_name, const char* input_json) {
    if (!trace_is_enabled()) return;

    // Build data string
    char* data = NULL;
    if (tool_name) {
        size_t len = strlen(tool_name) + (input_json ? strlen(input_json) : 2) + 32;
        data = malloc(len);
        snprintf(data, len, "{\"tool\":\"%s\",\"input\":%s}",
                 tool_name, input_json ? input_json : "{}");
    }

    TraceEvent event = {
        .type = TRACE_TOOL_CALL,
        .timestamp_ms = trace_get_time_ms(),
        .agent_id = agent_id,
        .agent_name = agent_name ? strdup(agent_name) : NULL,
        .data = data,
        .is_error = false
    };

    trace_emit(&event);

    free(event.agent_name);
    free(event.data);
}

void trace_tool_result(uint32_t agent_id, const char* agent_name,
                       const char* tool_name, const char* result) {
    if (!trace_is_enabled()) return;

    // Build data string (truncate result if too long)
    char* data = NULL;
    if (tool_name) {
        size_t result_len = result ? strlen(result) : 0;
        bool truncated = result_len > 200;
        size_t len = strlen(tool_name) + (truncated ? 200 : result_len) + 64;
        data = malloc(len);

        if (truncated) {
            char truncated_result[204];
            snprintf(truncated_result, sizeof(truncated_result), "%.200s...", result);
            snprintf(data, len, "{\"tool\":\"%s\",\"result\":\"%s\",\"truncated\":true}",
                     tool_name, truncated_result);
        } else {
            snprintf(data, len, "{\"tool\":\"%s\",\"bytes\":%zu}",
                     tool_name, result_len);
        }
    }

    TraceEvent event = {
        .type = TRACE_TOOL_RESULT,
        .timestamp_ms = trace_get_time_ms(),
        .agent_id = agent_id,
        .agent_name = agent_name ? strdup(agent_name) : NULL,
        .data = data,
        .is_error = false
    };

    trace_emit(&event);

    free(event.agent_name);
    free(event.data);
}

void trace_http_start(const char* url, const char* method) {
    if (!trace_is_enabled()) return;

    char data[512];
    snprintf(data, sizeof(data), "{\"method\":\"%s\",\"url\":\"%s\"}",
             method ? method : "POST",
             url ? url : "unknown");

    TraceEvent event = {
        .type = TRACE_HTTP_START,
        .timestamp_ms = trace_get_time_ms(),
        .data = strdup(data),
        .is_error = false
    };

    trace_emit(&event);

    free(event.data);
}

void trace_http_done(int status_code, uint64_t duration_ms,
                     TokenUsage* tokens, const char* error) {
    if (!trace_is_enabled()) return;

    TraceEvent event = {
        .type = TRACE_HTTP_DONE,
        .timestamp_ms = trace_get_time_ms(),
        .status_code = status_code,
        .duration_ms = duration_ms,
        .data = error ? strdup(error) : NULL,
        .is_error = (error != NULL || status_code >= 400)
    };

    if (tokens) {
        event.tokens = *tokens;
    }

    trace_emit(&event);

    free(event.data);
}

void trace_error(uint32_t agent_id, const char* message) {
    if (!trace_is_enabled()) return;

    TraceEvent event = {
        .type = TRACE_ERROR,
        .timestamp_ms = trace_get_time_ms(),
        .agent_id = agent_id,
        .data = message ? strdup(message) : NULL,
        .is_error = true
    };

    trace_emit(&event);

    free(event.data);
}

void trace_print(const char* text) {
    if (!trace_is_enabled()) return;

    TraceEvent event = {
        .type = TRACE_PRINT,
        .timestamp_ms = trace_get_time_ms(),
        .data = text ? strdup(text) : NULL,
        .is_error = false
    };

    trace_emit(&event);

    free(event.data);
}

// ============================================================================
// Utility
// ============================================================================

uint64_t trace_get_time_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

const char* trace_event_type_name(TraceEventType type) {
    switch (type) {
        case TRACE_AGENT_SPAWN: return "AGENT_SPAWN";
        case TRACE_AGENT_FREE:  return "AGENT_FREE";
        case TRACE_MSG_SEND:    return "MSG_SEND";
        case TRACE_MSG_RECV:    return "MSG_RECV";
        case TRACE_TOOL_CALL:   return "TOOL_CALL";
        case TRACE_TOOL_RESULT: return "TOOL_RESULT";
        case TRACE_HTTP_START:  return "HTTP_START";
        case TRACE_HTTP_DONE:   return "HTTP_DONE";
        case TRACE_ERROR:       return "ERROR";
        case TRACE_VM_STEP:     return "VM_STEP";
        case TRACE_PRINT:       return "PRINT";
        default:                return "UNKNOWN";
    }
}

void trace_event_free(TraceEvent* event) {
    if (!event) return;
    free(event->agent_name);
    free(event->data);
    // Note: Don't free the event struct itself - caller manages that
}

TraceEvent* trace_event_copy(TraceEvent* event) {
    if (!event) return NULL;

    TraceEvent* copy = malloc(sizeof(TraceEvent));
    if (!copy) return NULL;

    *copy = *event;
    copy->agent_name = event->agent_name ? strdup(event->agent_name) : NULL;
    copy->data = event->data ? strdup(event->data) : NULL;

    return copy;
}
