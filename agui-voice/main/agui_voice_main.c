// AG-UI on-device voice client — application entry point.
// P0/P0.5: WiFi (multi-SSID + captive portal). P1: streaming STT (Soniox). P2: AG-UI client
// (final transcript → POST RunAgentInput → stream SSE → reply text on serial). Turn-taking:
// STT is paused while a run is in flight (sequential TLS, per the plan). See
// docs/agui-voice-plan.md for the roadmap.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"

#include "net_prov.h"
#include "app_cfg.h"
#include "soniox_client.h"
#include "agui_client.h"
#include "device_tools.h"
#include "chat_ui.h"

static const char *TAG = "agui_voice";

static QueueHandle_t s_turn_q;   // final transcripts (char*) → agent task

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// --- Soniox transcript callbacks (fire from the websocket task; keep them fast) ---
static void on_partial(const char *text, void *ctx)
{
    ESP_LOGI("stt", "~ %s", text);          // live interim transcript
    chat_ui_status(text);                    // live feedback in the status line
}
static void on_turn(const char *text, void *ctx)
{
    if (!text) return;
    while (*text == ' ' || *text == '\t') text++;   // ignore empty/whitespace turns
    if (!*text) return;
    char *dup = strdup(text);                // handed to the agent task
    if (dup && xQueueSend(s_turn_q, &dup, 0) != pdTRUE) free(dup);   // drop if backlogged
}

// --- AG-UI run handlers (fire from the agent task) ---
static bool s_assist_started;   // has the assistant bubble been created for this run yet?

static void h_run_started(void *c)  { ESP_LOGI("agui", "thinking…"); printf("\nAI: "); fflush(stdout); }
static void h_text_delta(const char *d, void *c)
{
    if (!s_assist_started) { chat_ui_begin_assistant(); s_assist_started = true; }  // lazily, on first text
    chat_ui_append_assistant(d);
    printf("%s", d); fflush(stdout);
}
static void h_text_end(void *c)     { printf("\n"); fflush(stdout); }
static void h_reasoning(const char *d, bool active, void *c) { if (d && *d) ESP_LOGI("agui", "reasoning: %s", d); }
static void h_tool_call(const char *id, const char *name, const char *args, void *c)
{ ESP_LOGI("agui", "TOOL_CALL %s (%s) args=%s", name, id, args); }
static void h_run_finished(void *c) { ESP_LOGI("agui", "run finished"); chat_ui_status("Ready"); }
static void h_error(const char *m, void *c) { ESP_LOGE("agui", "run error: %s", m); chat_ui_status("Error"); }

static const agui_handlers_t s_handlers = {
    .on_run_started  = h_run_started,
    .on_text_delta   = h_text_delta,
    .on_text_end     = h_text_end,
    .on_reasoning    = h_reasoning,
    .on_tool_call    = h_tool_call,
    .on_run_finished = h_run_finished,
    .on_error        = h_error,
};

// Pause STT, run one AG-UI turn (streams the reply), then resume STT. One run at a time.
static void agent_task(void *arg)
{
    char url[APP_CFG_VAL_MAX], token[APP_CFG_VAL_MAX];
    for (;;) {
        char *text = NULL;
        if (xQueueReceive(s_turn_q, &text, portMAX_DELAY) != pdTRUE || !text) continue;
        ESP_LOGI(TAG, "You: %s", text);
        chat_ui_add_user(text);

        if (!app_cfg_get(APP_CFG_AGUI_URL, url, sizeof url)) {
            ESP_LOGW(TAG, "no AG-UI URL configured — skipping run");
            chat_ui_status("No AG-UI URL");
            free(text);
            continue;
        }
        bool have_tok = app_cfg_get(APP_CFG_AGUI_TOKEN, token, sizeof token);

        s_assist_started = false;            // fresh assistant bubble for this run
        chat_ui_status("Thinking…");
        soniox_session_stop();               // free the STT TLS session for the duration of the run
        agui_cfg_t acfg = {
            .endpoint    = url,
            .auth_bearer = have_tok ? token : NULL,
            .thread_id   = NULL,             // client keeps a persistent threadId
        };
        agui_run(&acfg, text, NULL, NULL, NULL, &s_handlers, NULL);
        free(text);

        soniox_cfg_t scfg = { 0 };           // resume listening
        if (soniox_session_start(&scfg, on_partial, on_turn, NULL) != ESP_OK)
            ESP_LOGE(TAG, "failed to resume STT after run");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AG-UI voice client booting (P0/P0.5/P1/P2)");
    init_nvs();

    // Non-network components (UI/tools stubs until later phases).
    device_tools_init();
    chat_ui_init();
    agui_client_init();

    // Provision until we have WiFi + Soniox key + AG-UI URL. The portal collects any of them;
    // on first P2 boot WiFi+key may already be saved, so it opens just to capture the AG-UI URL.
    ESP_ERROR_CHECK(net_prov_init());
    for (;;) {
        bool wifi_ok = net_is_connected() || (net_connect_saved(15000) == ESP_OK);
        bool key_ok  = app_cfg_has(APP_CFG_SONIOX_KEY);
        bool url_ok  = app_cfg_has(APP_CFG_AGUI_URL);
        if (wifi_ok && key_ok && url_ok) break;
        ESP_LOGW(TAG, "provisioning needed (wifi=%d, soniox_key=%d, agui_url=%d) — opening 'AMOLED-setup'",
                 wifi_ok, key_ok, url_ok);
        net_portal_start("AMOLED-setup");
        net_portal_wait_saved(0);   // block until the user submits the form
        net_portal_stop();
    }
    net_start_auto_reconnect();
    ESP_LOGI(TAG, "network + Soniox key + AG-UI URL ready");

    // P2: agent turn-taking. Final transcripts flow STT → queue → agent task → AG-UI run.
    s_turn_q = xQueueCreate(4, sizeof(char *));
    xTaskCreate(agent_task, "agui_agent", 12288, NULL, 5, NULL);   // TLS handshake runs on this stack

    // Start streaming STT. Speak; Soniox endpoint-detection commits a turn ("<end>") which
    // the agent task sends to the AG-UI agent and streams the reply on the console.
    if (soniox_client_init() == ESP_OK) {
        soniox_cfg_t scfg = { 0 };   // defaults; api_key read from NVS
        if (soniox_session_start(&scfg, on_partial, on_turn, NULL) == ESP_OK)
            ESP_LOGI(TAG, "Soniox session started — speak into the mic");
        else
            ESP_LOGE(TAG, "Soniox session failed to start");
    }

    // Heartbeat: report link/heap, and auto-recover STT from a fatal Soniox error.
    char ip[16];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        const char *stt_err = soniox_last_error();
        bool online = net_get_ip_str(ip, sizeof(ip));
        if (online) {
            ESP_LOGI(TAG, "heartbeat: online ip=%s  stt=%d  free_heap=%u  psram=%u",
                     ip, soniox_session_active(), (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        } else {
            ESP_LOGW(TAG, "heartbeat: offline  free_heap=%u",
                     (unsigned)esp_get_free_heap_size());
        }
        // A fatal Soniox error (e.g. "Request timeout" on a transient drop) leaves the session
        // dead; tear it down and start a fresh one so STT self-heals without a reboot. Only when
        // online (it needs the network); a clean turn never sets an error, so this won't fight the
        // agent task's stop/start. A persistently bad key just retries each heartbeat (~10 s).
        if (stt_err && online) {
            ESP_LOGW(TAG, "STT down (Soniox: %s) — restarting session", stt_err);
            soniox_session_stop();
            soniox_cfg_t scfg = { 0 };   // api_key re-read from NVS
            if (soniox_session_start(&scfg, on_partial, on_turn, NULL) == ESP_OK)
                ESP_LOGI(TAG, "STT session restarted");
            else
                ESP_LOGW(TAG, "STT restart failed — retrying next heartbeat");
        }
    }
}
