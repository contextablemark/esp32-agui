// agui_client — on-device AG-UI client (P2: text). See include/agui_client.h.
//
// POST RunAgentInput (JSON) → stream the Server-Sent-Events response → route events to
// handler callbacks. Ported from the ag-ui C++ SDK idea (libcurl→esp_http_client,
// nlohmann→cJSON). v1 handles the chat/status event subset; STATE_*/JSON-Patch and the
// interrupt outcome are deferred (v2 / P6). Wire format (verified via the protocol repo):
//   - the SSE event type is a `type` field INSIDE the data: JSON (the SSE event: field is
//     unused); TEXT_MESSAGE_CONTENT carries the text in `delta`; context value is a string.
//
// One run at a time (sequential TLS, per the plan): a mutex serializes agui_run().

#include "agui_client.h"
#include "app_cfg.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_random.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "agui_client";

#define RUN_TIMEOUT_MS   45000     // socket timeout: tolerate a slow-thinking agent
#define READ_CHUNK       1024      // SSE read granularity
#define LINE_CAP         4096      // max single SSE line (longer lines are dropped)
#define DATA_CAP         16384     // max accumulated event payload
#define ASSIST_CAP       4096      // assistant text kept for history (UI sees every delta)
#define TARGS_CAP        2048      // accumulated tool-call args
#define MAX_HISTORY      20        // messages retained client-side (oldest pruned)

static SemaphoreHandle_t s_lock;          // serializes runs
static cJSON            *s_messages;       // conversation history (owned)
static char              s_thread_id[40];  // persistent across runs (conversation continuity)

// ---- per-run state (one run at a time under s_lock) -----------------------

typedef struct {
    const agui_handlers_t *h;
    void  *ctx;
    char  *line;  size_t line_len;  bool line_ovf;   // current SSE line
    char  *data;  size_t data_len;  bool data_ovf;   // accumulated data: payload
    char  *assist; size_t assist_len;                // current assistant message text
    char  *targs;  size_t targs_len;                 // current tool-call args
    char   msg_id[64];
    char   tool_id[64];
    char   tool_name[80];
    bool   finished;                                 // RUN_FINISHED / RUN_ERROR seen
} run_t;

// ---- helpers --------------------------------------------------------------

static void gen_id(const char *prefix, char *buf, size_t n)
{
    uint32_t a = esp_random(), b = esp_random();
    snprintf(buf, n, "%s_%08x%08x", prefix, (unsigned)a, (unsigned)b);
}

static void hist_append(const char *role, const char *content, const char *id)
{
    if (!s_messages) s_messages = cJSON_CreateArray();
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "id", id);
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "content", content);
    cJSON_AddItemToArray(s_messages, m);
    while (cJSON_GetArraySize(s_messages) > MAX_HISTORY)
        cJSON_DeleteItemFromArray(s_messages, 0);   // prune oldest
}

// Build the RunAgentInput JSON body. s_messages must already hold the user turn.
static char *build_body(const char *run_id, const cJSON *context,
                        const cJSON *tools, const cJSON *resume)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "threadId", s_thread_id);
    cJSON_AddStringToObject(root, "runId", run_id);
    cJSON_AddItemToObject(root, "messages",
                          s_messages ? cJSON_Duplicate(s_messages, true) : cJSON_CreateArray());
    cJSON_AddItemToObject(root, "tools",
                          tools ? cJSON_Duplicate((cJSON *)tools, true) : cJSON_CreateArray());
    cJSON_AddItemToObject(root, "context",
                          context ? cJSON_Duplicate((cJSON *)context, true) : cJSON_CreateArray());
    cJSON_AddItemToObject(root, "state", cJSON_CreateObject());
    // forwardedProps carries the interrupt resume payload in P6; empty for now.
    cJSON *fwd = cJSON_CreateObject();
    if (resume) cJSON_AddItemToObject(fwd, "resume", cJSON_Duplicate((cJSON *)resume, true));
    cJSON_AddItemToObject(root, "forwardedProps", fwd);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return s;
}

// ---- event routing (runs in the caller's task) ----------------------------

static void route(run_t *r, const char *t, cJSON *ev)
{
    const agui_handlers_t *h = r->h;
    void *ctx = r->ctx;

    if (!strcmp(t, "RUN_STARTED")) {
        if (h->on_run_started) h->on_run_started(ctx);

    } else if (!strcmp(t, "TEXT_MESSAGE_START")) {
        cJSON *mid = cJSON_GetObjectItem(ev, "messageId");
        strlcpy(r->msg_id, cJSON_IsString(mid) ? mid->valuestring : "", sizeof r->msg_id);
        r->assist_len = 0; r->assist[0] = '\0';

    } else if (!strcmp(t, "TEXT_MESSAGE_CONTENT")) {
        cJSON *d = cJSON_GetObjectItem(ev, "delta");
        if (cJSON_IsString(d) && d->valuestring) {
            if (h->on_text_delta) h->on_text_delta(d->valuestring, ctx);
            size_t dl = strlen(d->valuestring);
            if (r->assist_len + dl < ASSIST_CAP) {
                memcpy(r->assist + r->assist_len, d->valuestring, dl);
                r->assist_len += dl;
                r->assist[r->assist_len] = '\0';
            }
        }

    } else if (!strcmp(t, "TEXT_MESSAGE_END")) {
        if (h->on_text_end) h->on_text_end(ctx);
        if (r->assist_len) {
            if (!r->msg_id[0]) gen_id("msg", r->msg_id, sizeof r->msg_id);
            hist_append("assistant", r->assist, r->msg_id);
        }
        r->assist_len = 0;

    } else if (!strncmp(t, "REASONING_", 10)) {
        cJSON *d = cJSON_GetObjectItem(ev, "delta");
        bool active = strcmp(t, "REASONING_END") != 0;
        if (h->on_reasoning)
            h->on_reasoning(cJSON_IsString(d) ? d->valuestring : NULL, active, ctx);

    } else if (!strcmp(t, "TOOL_CALL_START")) {
        cJSON *id = cJSON_GetObjectItem(ev, "toolCallId");
        cJSON *nm = cJSON_GetObjectItem(ev, "toolCallName");
        strlcpy(r->tool_id, cJSON_IsString(id) ? id->valuestring : "", sizeof r->tool_id);
        strlcpy(r->tool_name, cJSON_IsString(nm) ? nm->valuestring : "", sizeof r->tool_name);
        r->targs_len = 0; r->targs[0] = '\0';

    } else if (!strcmp(t, "TOOL_CALL_ARGS")) {
        cJSON *d = cJSON_GetObjectItem(ev, "delta");
        if (cJSON_IsString(d) && d->valuestring) {
            size_t dl = strlen(d->valuestring);
            if (r->targs_len + dl < TARGS_CAP) {
                memcpy(r->targs + r->targs_len, d->valuestring, dl);
                r->targs_len += dl;
                r->targs[r->targs_len] = '\0';
            }
        }

    } else if (!strcmp(t, "TOOL_CALL_END")) {
        if (h->on_tool_call)
            h->on_tool_call(r->tool_id, r->tool_name, r->targs_len ? r->targs : "{}", ctx);

    } else if (!strcmp(t, "RUN_FINISHED")) {
        // Interrupt outcome (RUN_FINISHED outcome=interrupt) is parsed in P6.
        if (h->on_run_finished) h->on_run_finished(ctx);
        r->finished = true;

    } else if (!strcmp(t, "RUN_ERROR")) {
        cJSON *m = cJSON_GetObjectItem(ev, "message");
        if (h->on_error) h->on_error(cJSON_IsString(m) ? m->valuestring : "run error", ctx);
        r->finished = true;
    }
    // STATE_* / MESSAGES_SNAPSHOT / ACTIVITY_* — deferred to v2.
}

static void sse_dispatch(run_t *r, const char *json)
{
    cJSON *ev = cJSON_Parse(json);
    if (!ev) { ESP_LOGW(TAG, "unparseable SSE event"); return; }
    cJSON *jt = cJSON_GetObjectItem(ev, "type");
    if (cJSON_IsString(jt) && jt->valuestring) route(r, jt->valuestring, ev);
    cJSON_Delete(ev);
}

// Process one complete (NUL-terminated) SSE line.
static void sse_line(run_t *r)
{
    if (r->line_len == 0) {                 // blank line: dispatch the accumulated event
        if (r->data_ovf) {
            ESP_LOGW(TAG, "dropped oversized SSE event");
        } else if (r->data_len) {
            r->data[r->data_len] = '\0';
            sse_dispatch(r, r->data);
        }
        r->data_len = 0; r->data_ovf = false;
        return;
    }
    if (r->line[0] == ':') return;          // comment
    if (strncmp(r->line, "data:", 5) == 0) {
        const char *v = r->line + 5;
        if (*v == ' ') v++;                  // optional single space after colon
        size_t vl = strlen(v);
        if (r->data_len && r->data_len + 1 < DATA_CAP) r->data[r->data_len++] = '\n'; // join multi-line data
        if (r->data_len + vl < DATA_CAP) {
            memcpy(r->data + r->data_len, v, vl);
            r->data_len += vl;
        } else {
            r->data_ovf = true;
        }
    }
    // event:/id:/retry: ignored — the type lives in the data JSON.
}

static void sse_feed(run_t *r, const char *buf, int n)
{
    for (int i = 0; i < n; i++) {
        char c = buf[i];
        if (c == '\r') continue;
        if (c == '\n') {
            if (r->line_ovf) ESP_LOGW(TAG, "dropped oversized SSE line");
            else { r->line[r->line_len] = '\0'; sse_line(r); }
            r->line_len = 0; r->line_ovf = false;
            continue;
        }
        if (r->line_len + 1 < LINE_CAP) r->line[r->line_len++] = c;
        else r->line_ovf = true;
    }
}

// ---- public API -----------------------------------------------------------

esp_err_t agui_client_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_thread_id[0]) gen_id("thread", s_thread_id, sizeof s_thread_id);
    ESP_LOGI(TAG, "init (thread %s)", s_thread_id);
    return ESP_OK;
}

esp_err_t agui_run(const agui_cfg_t *cfg, const char *user_text,
                   const cJSON *context, const cJSON *tools, const cJSON *resume,
                   const agui_handlers_t *handlers, void *ctx)
{
    if (!cfg || !cfg->endpoint || !handlers) return ESP_ERR_INVALID_ARG;
    if (!s_lock) agui_client_init();
    xSemaphoreTake(s_lock, portMAX_DELAY);

    if (cfg->thread_id && cfg->thread_id[0])
        strlcpy(s_thread_id, cfg->thread_id, sizeof s_thread_id);

    char run_id[40];
    gen_id("run", run_id, sizeof run_id);

    // Append the user turn to history (skip when resuming an interrupt).
    if (user_text && user_text[0] && !resume) {
        char uid[40];
        gen_id("msg", uid, sizeof uid);
        hist_append("user", user_text, uid);
    }

    char *body = build_body(run_id, context, tools, resume);
    if (!body) { xSemaphoreGive(s_lock); return ESP_ERR_NO_MEM; }

    // Per-run scratch buffers in PSRAM.
    run_t r = { .h = handlers, .ctx = ctx };
    r.line   = heap_caps_malloc(LINE_CAP,   MALLOC_CAP_SPIRAM);
    r.data   = heap_caps_malloc(DATA_CAP,   MALLOC_CAP_SPIRAM);
    r.assist = heap_caps_malloc(ASSIST_CAP, MALLOC_CAP_SPIRAM);
    r.targs  = heap_caps_malloc(TARGS_CAP,  MALLOC_CAP_SPIRAM);
    if (!r.line || !r.data || !r.assist || !r.targs) {
        heap_caps_free(r.line); heap_caps_free(r.data);
        heap_caps_free(r.assist); heap_caps_free(r.targs);
        cJSON_free(body);
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    esp_http_client_config_t hcfg = {
        .url               = cfg->endpoint,
        .method            = HTTP_METHOD_POST,
        .timeout_ms        = RUN_TIMEOUT_MS,
        .buffer_size       = 2048,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t client = esp_http_client_init(&hcfg);
    esp_err_t ret = ESP_FAIL;
    if (!client) { if (handlers->on_error) handlers->on_error("http init failed", ctx); goto done; }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Accept", "text/event-stream");
    if (cfg->auth_bearer && cfg->auth_bearer[0]) {
        char auth[APP_CFG_VAL_MAX + 8];
        snprintf(auth, sizeof auth, "Bearer %s", cfg->auth_bearer);
        esp_http_client_set_header(client, "Authorization", auth);
    }

    int blen = strlen(body);
    esp_err_t oe = esp_http_client_open(client, blen);
    if (oe != ESP_OK) {
        ESP_LOGE(TAG, "connect failed: %s", esp_err_to_name(oe));
        if (handlers->on_error) handlers->on_error(esp_err_to_name(oe), ctx);
        goto done;
    }
    for (int off = 0; off < blen; ) {
        int w = esp_http_client_write(client, body + off, blen - off);
        if (w < 0) { ESP_LOGE(TAG, "request write failed"); if (handlers->on_error) handlers->on_error("write failed", ctx); goto done; }
        off += w;
    }
    esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status < 200 || status >= 300) {
        char msg[64];
        snprintf(msg, sizeof msg, "HTTP %d from agent", status);
        ESP_LOGE(TAG, "%s", msg);
        if (handlers->on_error) handlers->on_error(msg, ctx);
        goto done;
    }
    ESP_LOGI(TAG, "run %s streaming (HTTP %d)", run_id, status);

    // Stream the SSE body until the server closes it or the run finishes.
    char *chunk = heap_caps_malloc(READ_CHUNK, MALLOC_CAP_SPIRAM);
    if (chunk) {
        while (!r.finished) {
            int n = esp_http_client_read(client, chunk, READ_CHUNK);
            if (n <= 0) break;              // EOF (stream closed) or read error
            sse_feed(&r, chunk, n);
        }
        heap_caps_free(chunk);
        // Flush a final event that arrived without its trailing blank line (abrupt close).
        if (!r.finished && !r.line_ovf && !r.data_ovf) {
            if (r.line_len) { r.line[r.line_len] = '\0'; sse_line(&r); }
            if (r.data_len) { r.data[r.data_len] = '\0'; sse_dispatch(&r, r.data); }
        }
    }
    ret = ESP_OK;

done:
    if (client) { esp_http_client_close(client); esp_http_client_cleanup(client); }
    heap_caps_free(r.line); heap_caps_free(r.data);
    heap_caps_free(r.assist); heap_caps_free(r.targs);
    cJSON_free(body);
    xSemaphoreGive(s_lock);
    return ret;
}

esp_err_t agui_tool_result(const char *tool_call_id, const cJSON *result)
{
    // Client-side tool results are sent back as a follow-up run in P7.
    (void)tool_call_id; (void)result;
    return ESP_ERR_NOT_SUPPORTED;
}
