// soniox_client — see include/soniox_client.h and docs/soniox-rt-protocol.md.

#include "soniox_client.h"
#include "app_cfg.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "esp_websocket_client.h"
#include "esp_crt_bundle.h"
#include "esp_codec_dev.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"

static const char *TAG = "soniox";

#define DEFAULT_ENDPOINT "wss://stt-rt.soniox.com/transcribe-websocket"
#define DEFAULT_MODEL    "stt-rt-v5"
#define DEFAULT_SR       16000
#define MIC_GAIN_DB      30.0f
#define PCM_CHUNK_BYTES  3840          // 1920 samples = 120 ms @ 16k/mono/s16le
#define COMMITTED_MAX    512
#define RUNNING_MAX      640
#define MSG_MAX          32768         // reject pathologically large server messages

static esp_codec_dev_handle_t        s_mic;
static esp_websocket_client_handle_t s_ws;
static SemaphoreHandle_t             s_lock;       // serializes start/finalize/stop
static SemaphoreHandle_t             s_cap_done;   // capture task signals exit
static TaskHandle_t                  s_cap_task;
static volatile bool                 s_stop;
static volatile bool                 s_fatal;      // fatal Soniox error -> session dead
static volatile bool                 s_need_config;// (re)send config after (re)connect
static volatile bool                 s_active;

static soniox_partial_cb s_partial_cb;
static soniox_turn_cb    s_turn_cb;
static void             *s_ctx;

static char s_api_key[APP_CFG_VAL_MAX];
static char s_model[32];
static int  s_sr;
static char s_committed[COMMITTED_MAX];   // touched only from the ws task
static char s_last_error[128];

// reassembly of multi-event messages (PSRAM)
static uint8_t *s_rx;
static size_t   s_rx_total;

// ---- transcript parsing (runs in ws task) --------------------------------

static void parse_message(const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (!root) return;

    cJSON *errm = cJSON_GetObjectItem(root, "error_message");
    if (cJSON_IsString(errm)) {
        strlcpy(s_last_error, errm->valuestring, sizeof(s_last_error));
        s_fatal = true;                  // stop streaming; surface to the app
        ESP_LOGE(TAG, "soniox error: %s", errm->valuestring);
        cJSON_Delete(root);
        return;
    }

    char tail[RUNNING_MAX];
    tail[0] = '\0';

    cJSON *toks = cJSON_GetObjectItem(root, "tokens");
    if (cJSON_IsArray(toks)) {
        cJSON *t;
        cJSON_ArrayForEach(t, toks) {
            cJSON *txt = cJSON_GetObjectItem(t, "text");
            if (!cJSON_IsString(txt) || !txt->valuestring) continue;
            const char *s = txt->valuestring;
            bool is_final = cJSON_IsTrue(cJSON_GetObjectItem(t, "is_final"));
            if (is_final) {
                if (strcmp(s, "<end>") == 0) {       // utterance boundary
                    if (s_turn_cb) s_turn_cb(s_committed, s_ctx);
                    s_committed[0] = '\0';
                } else {
                    strlcat(s_committed, s, sizeof(s_committed));
                }
            } else {
                strlcat(tail, s, sizeof(tail));
            }
        }
    }

    if (s_partial_cb) {
        char running[RUNNING_MAX];
        snprintf(running, sizeof(running), "%s%s", s_committed, tail);
        if (running[0]) s_partial_cb(running, s_ctx);   // skip empty keepalive messages
    }

    if (cJSON_IsTrue(cJSON_GetObjectItem(root, "finished")))
        ESP_LOGI(TAG, "server finished session");

    cJSON_Delete(root);
}

// ---- websocket events ----------------------------------------------------

static void ws_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_websocket_event_data_t *e = data;
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "ws connected");
        s_committed[0] = '\0';      // fresh transcript context (don't splice across drops)
        s_need_config = true;
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        if (s_rx) { heap_caps_free(s_rx); s_rx = NULL; }
        break;
    case WEBSOCKET_EVENT_DATA: {
        if (!e || e->payload_len == 0) break;
        if (e->op_code == 0x08 || e->op_code == 0x09 || e->op_code == 0x0A) break; // close/ping/pong
        if (e->payload_len > MSG_MAX) { ESP_LOGW(TAG, "oversized msg %d, dropped", (int)e->payload_len); break; }
        if (e->payload_offset == 0) {                 // first chunk of a message
            if (s_rx) heap_caps_free(s_rx);
            s_rx = heap_caps_malloc(e->payload_len + 1, MALLOC_CAP_SPIRAM);
            s_rx_total = e->payload_len;
        }
        if (!s_rx) break;
        if (e->payload_offset + e->data_len <= s_rx_total)
            memcpy(s_rx + e->payload_offset, e->data_ptr, e->data_len);
        if (e->payload_offset + e->data_len >= s_rx_total) {   // message complete
            s_rx[s_rx_total] = '\0';
            parse_message((const char *)s_rx);
            heap_caps_free(s_rx);
            s_rx = NULL;
        }
        break;
    }
    case WEBSOCKET_EVENT_ERROR:
        ESP_LOGW(TAG, "ws transport error");
        break;
    default: break;
    }
}

// ---- config frame --------------------------------------------------------

static esp_err_t send_config_frame(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "api_key", s_api_key);
    cJSON_AddStringToObject(root, "model", s_model);
    cJSON_AddStringToObject(root, "audio_format", "pcm_s16le");
    cJSON_AddNumberToObject(root, "sample_rate", s_sr);
    cJSON_AddNumberToObject(root, "num_channels", 1);
    cJSON *lh = cJSON_AddArrayToObject(root, "language_hints");
    cJSON_AddItemToArray(lh, cJSON_CreateString("en"));
    cJSON_AddBoolToObject(root, "enable_endpoint_detection", true);

    char *s = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!s) return ESP_ERR_NO_MEM;
    int n = esp_websocket_client_send_text(s_ws, s, strlen(s), pdMS_TO_TICKS(2000));
    cJSON_free(s);
    ESP_LOGI(TAG, "sent config frame (%d)", n);
    return n < 0 ? ESP_FAIL : ESP_OK;
}

// ---- capture task: mic -> binary WS frames -------------------------------

static void capture_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(PCM_CHUNK_BYTES, MALLOC_CAP_DEFAULT);
    if (buf) {
        while (!s_stop && !s_fatal) {
            if (!esp_websocket_client_is_connected(s_ws)) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
            if (s_need_config) {
                if (send_config_frame() == ESP_OK) s_need_config = false;
                else { vTaskDelay(pdMS_TO_TICKS(200)); continue; }
            }
            if (esp_codec_dev_read(s_mic, buf, PCM_CHUNK_BYTES) == ESP_OK &&
                !s_stop && esp_websocket_client_is_connected(s_ws)) {
                esp_websocket_client_send_bin(s_ws, (const char *)buf, PCM_CHUNK_BYTES, pdMS_TO_TICKS(500));
            }
        }
        heap_caps_free(buf);
    } else {
        ESP_LOGE(TAG, "no mem for capture buf");
    }
    // From here on we no longer touch s_ws — safe for stop() to destroy it.
    s_cap_task = NULL;
    xSemaphoreGive(s_cap_done);
    vTaskDelete(NULL);
}

// ---- public API ----------------------------------------------------------

esp_err_t soniox_client_init(void)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_cap_done) s_cap_done = xSemaphoreCreateBinary();
    if (s_mic) return ESP_OK;
    s_mic = bsp_audio_codec_microphone_init();
    if (!s_mic) { ESP_LOGE(TAG, "mic init failed"); return ESP_FAIL; }
    esp_codec_dev_set_in_gain(s_mic, MIC_GAIN_DB);
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .sample_rate = DEFAULT_SR,
    };
    esp_err_t err = esp_codec_dev_open(s_mic, &fs);
    if (err != ESP_OK) { ESP_LOGE(TAG, "mic open failed: %s", esp_err_to_name(err)); return err; }
    ESP_LOGI(TAG, "mic ready (16k mono)");
    return ESP_OK;
}

esp_err_t soniox_session_start(const soniox_cfg_t *cfg,
                               soniox_partial_cb on_partial,
                               soniox_turn_cb on_turn, void *ctx)
{
    if (!s_mic) { esp_err_t e = soniox_client_init(); if (e != ESP_OK) return e; }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_active) { xSemaphoreGive(s_lock); return ESP_ERR_INVALID_STATE; }

    const char *endpoint = (cfg && cfg->endpoint) ? cfg->endpoint : DEFAULT_ENDPOINT;
    strlcpy(s_model, (cfg && cfg->model) ? cfg->model : DEFAULT_MODEL, sizeof(s_model));
    s_sr = (cfg && cfg->sample_rate) ? cfg->sample_rate : DEFAULT_SR;
    if (cfg && cfg->api_key) {
        strlcpy(s_api_key, cfg->api_key, sizeof(s_api_key));
    } else if (!app_cfg_get(APP_CFG_SONIOX_KEY, s_api_key, sizeof(s_api_key))) {
        ESP_LOGE(TAG, "no usable Soniox API key (missing or too long) — re-enter via the portal");
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    s_partial_cb = on_partial;
    s_turn_cb = on_turn;
    s_ctx = ctx;
    s_committed[0] = '\0';
    s_last_error[0] = '\0';
    s_stop = false;
    s_fatal = false;
    s_need_config = true;
    xSemaphoreTake(s_cap_done, 0);   // drain any stale signal from a prior session

    esp_websocket_client_config_t wcfg = {
        .uri = endpoint,
        .buffer_size = 4096,
        .task_stack = 8192,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .reconnect_timeout_ms = 5000,
        .network_timeout_ms = 10000,
        .ping_interval_sec = 20,
    };
    s_ws = esp_websocket_client_init(&wcfg);
    if (!s_ws) { xSemaphoreGive(s_lock); return ESP_FAIL; }
    esp_websocket_register_events(s_ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);
    esp_err_t err = esp_websocket_client_start(s_ws);
    if (err != ESP_OK) { esp_websocket_client_destroy(s_ws); s_ws = NULL; xSemaphoreGive(s_lock); return err; }

    s_active = true;
    if (xTaskCreate(capture_task, "soniox_cap", 4096, NULL, 5, &s_cap_task) != pdPASS) {
        esp_websocket_client_stop(s_ws);
        esp_websocket_client_destroy(s_ws);
        s_ws = NULL;
        s_active = false;
        xSemaphoreGive(s_lock);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "session started (%s, %s, %d Hz)", endpoint, s_model, s_sr);
    xSemaphoreGive(s_lock);
    return ESP_OK;
}

esp_err_t soniox_session_finalize(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_active && s_ws) {
        const char *msg = "{\"type\":\"finalize\"}";
        int n = esp_websocket_client_send_text(s_ws, msg, strlen(msg), pdMS_TO_TICKS(1000));
        ret = n < 0 ? ESP_FAIL : ESP_OK;
    }
    xSemaphoreGive(s_lock);
    return ret;
}

void soniox_session_stop(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_active) { xSemaphoreGive(s_lock); return; }
    s_stop = true;
    if (s_cap_task) xSemaphoreTake(s_cap_done, portMAX_DELAY);  // deterministic join
    esp_websocket_client_handle_t ws = s_ws;   // capture no longer touches s_ws
    s_ws = NULL;
    if (ws) {
        if (esp_websocket_client_is_connected(ws))
            esp_websocket_client_send_text(ws, "", 0, pdMS_TO_TICKS(500)); // end-of-audio
        esp_websocket_client_close(ws, pdMS_TO_TICKS(1000));
        esp_websocket_client_destroy(ws);
    }
    if (s_rx) { heap_caps_free(s_rx); s_rx = NULL; }
    s_active = false;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "session stopped");
}

bool soniox_session_active(void) { return s_active && !s_fatal; }

const char *soniox_last_error(void) { return s_last_error[0] ? s_last_error : NULL; }
