// AG-UI on-device voice client — application entry point.
// P0/P0.5: WiFi (multi-SSID + captive portal). P1: streaming STT (Soniox). P2: AG-UI client.
// P3: chat UI on the AMOLED + BOOT-button push-to-talk. See docs/agui-voice-plan.md.
//
// Turn model (P3 PTT): no continuous STT session. Hold the BOOT button → open the Soniox session
// and stream the mic (live transcript in the status line); release → stop the session and send the
// utterance to the AG-UI agent. Session is open only while held → no idle Soniox timeout, no
// streamed-silence garbage, and the button defines the turn boundary.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "iot_button.h"
#include "button_gpio.h"

#include "net_prov.h"
#include "app_cfg.h"
#include "soniox_client.h"
#include "agui_client.h"
#include "device_tools.h"
#include "chat_ui.h"

#include "esp_codec_dev.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"

static const char *TAG = "agui_voice";

#define PTT_GPIO    0                 // BOOT button: strapping pin at RESET only; normal input at runtime
#define IDLE_HINT   "Hold Top Button to talk"

static QueueHandle_t s_ptt_q;          // button events: 1 = press (down), 0 = release (up)
static volatile bool s_listening;      // a hold is in progress
static char s_ptt_final[512];          // finalized text accumulated across mid-hold endpoints
static char s_ptt_run[640];            // last interim ("running") transcript

static void init_nvs(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
}

// Apply the configured POSIX TZ (from the portal dropdown; default UTC0) so localtime_r() yields
// local time for the local_time ambient-context entry. Re-call after a reconfigure that may change it.
static void apply_timezone(void)
{
    char tz[64];
    setenv("TZ", (app_cfg_get(APP_CFG_TZ, tz, sizeof tz) && tz[0]) ? tz : "UTC0", 1);
    tzset();
}

// --- Soniox transcript callbacks (fire from the ws task; only meaningful while holding PTT) ---
static void on_partial(const char *text, void *ctx)
{
    if (!s_listening || !text) return;
    strlcpy(s_ptt_run, text, sizeof s_ptt_run);
    char live[1024];
    snprintf(live, sizeof live, "%s%s", s_ptt_final, text);   // finalized so far + current interim
    chat_ui_status(live[0] ? live : "Listening...");
    ESP_LOGI("stt", "~ %s", live);
}
static void on_turn(const char *text, void *ctx)             // Soniox endpoint mid-hold: keep the segment
{
    if (!s_listening || !text || !*text) return;
    strlcat(s_ptt_final, text, sizeof s_ptt_final);
    strlcat(s_ptt_final, " ", sizeof s_ptt_final);
    s_ptt_run[0] = '\0';
}

// --- AG-UI run handlers (fire from the ptt task during agui_run) ---
// They drive the chat bubbles (TEXT_*) and the ephemeral status line (RUN_*/REASONING_*/
// TOOL_CALL_*) — P4 live agent activity: status shows what the agent is doing and clears
// when the reply text starts streaming.
static bool s_assist_started;          // assistant bubble open for the current message?
static bool s_run_error;               // run ended in error → keep the "Error" status visible

// --- P7 client tools: pending list ------------------------------------------------------------
// Client-tool calls the agent makes during a run are RECORDED here by h_tool_call (which runs INSIDE
// agui_run under s_lock — so it may only copy strings, never dispatch/re-run). run_agent_turn drains
// the list AFTER agui_run returns, executes each tool, returns a result, then re-runs. Owned solely
// by ptt_task (single producer/consumer, never concurrent) → no locking.
#define AGUI_TOOL_MAX_ITERS 5      // hard cap on run→tool→run cycles per utterance
#define PEND_MAX            4      // max client tool calls captured per run
#define TOOL_ID_MAX        64
#define TOOL_NAME_MAX      48
#define TOOL_ARGS_MAX     512

typedef struct {
    char id[TOOL_ID_MAX];
    char name[TOOL_NAME_MAX];
    char args[TOOL_ARGS_MAX];      // raw JSON string from the SDK ("{}" if empty)
} pending_tool_t;

static pending_tool_t s_pending[PEND_MAX];
static int            s_pending_n;        // client tool calls recorded this run
static bool           s_pending_overflow;

static void h_run_started(void *c)
{
    ESP_LOGI("agui", "thinking...");
    chat_ui_status("Thinking...");
    printf("\nAI: "); fflush(stdout);
}
static void h_text_delta(const char *d, void *c)
{
    if (!s_assist_started) {                 // first delta of this message → open a bubble…
        chat_ui_begin_assistant();
        chat_ui_status("");                  // …and clear the reasoning/tool indicator
        s_assist_started = true;
    }
    chat_ui_append_assistant(d);
    printf("%s", d); fflush(stdout);
}
static void h_text_end(void *c)
{
    s_assist_started = false;                // message done → the next TEXT_MESSAGE gets its own bubble
    printf("\n"); fflush(stdout);
}
static void h_reasoning(const char *d, bool active, void *c)
{
    if (active) chat_ui_status("Reasoning...");   // ephemeral; cleared when the reply text starts
    if (d && *d) ESP_LOGI("agui", "reasoning: %s", d);
}
static void h_tool_call(const char *id, const char *name, const char *args, void *c)
{
    char s[96];
    snprintf(s, sizeof s, "Using %s...", (name && *name) ? name : "tool");
    chat_ui_status(s);                       // transient chip; the next event overwrites it
    ESP_LOGI("agui", "TOOL_CALL %s (%s) args=%s", name, id, args);

    // Record ONLY tools we can execute. Server/agent tools are run by the agent itself; we owe no
    // result for them, so we must not capture (and later try to answer) them. Runs inside agui_run
    // under s_lock → copy three strings and return; NO dispatch, NO re-run (would deadlock).
    if (!device_tools_is_client(name)) return;
    if (s_pending_n >= PEND_MAX) { s_pending_overflow = true; return; }
    pending_tool_t *p = &s_pending[s_pending_n++];
    strlcpy(p->id,   id   ? id   : "",                sizeof p->id);
    strlcpy(p->name, name ? name : "",                sizeof p->name);
    strlcpy(p->args, (args && args[0]) ? args : "{}", sizeof p->args);
}
static void h_run_finished(void *c) { ESP_LOGI("agui", "run finished"); }
static void h_error(const char *m, void *c)
{
    ESP_LOGE("agui", "run error: %s", m);
    chat_ui_status("Error");
    s_run_error = true;
}

static const agui_handlers_t s_handlers = {
    .on_run_started  = h_run_started,
    .on_text_delta   = h_text_delta,
    .on_text_end     = h_text_end,
    .on_reasoning    = h_reasoning,
    .on_tool_call    = h_tool_call,
    .on_run_finished = h_run_finished,
    .on_error        = h_error,
};

// Render the user turn, run one AG-UI turn (streams the reply into the chat), restore the idle hint.
static void run_agent_turn(const char *text)
{
    char url[APP_CFG_VAL_MAX], token[APP_CFG_VAL_MAX];
    ESP_LOGI(TAG, "You: %s", text);
    chat_ui_add_user(text);

    if (!app_cfg_get(APP_CFG_AGUI_URL, url, sizeof url)) {
        ESP_LOGW(TAG, "no AG-UI URL configured");
        chat_ui_status("No AG-UI URL");
        return;
    }
    bool have_tok = app_cfg_get(APP_CFG_AGUI_TOKEN, token, sizeof token);

    s_assist_started = false;
    s_run_error      = false;
    chat_ui_status("Thinking...");
    agui_cfg_t acfg = { .endpoint = url, .auth_bearer = have_tok ? token : NULL, .thread_id = NULL };
    cJSON *tools    = device_tools_manifest();     // advertised on EVERY run (NULL ⇒ no tools)

    // The agent may call device client tools (set_timer, ...). Advertise them each run; after each
    // run, drain any recorded calls — execute on-device, append a tool result to history, then re-run
    // with NULL user_text so the agent continues. Bounded by AGUI_TOOL_MAX_ITERS. Every agui_run /
    // agui_tool_result call happens HERE on ptt_task with s_lock free between them (no deadlock).
    const char *user_text = text;                  // first run carries the utterance; continuations don't
    for (int iter = 0; iter < AGUI_TOOL_MAX_ITERS; iter++) {
        s_pending_n        = 0;                    // reset capture for THIS run
        s_pending_overflow = false;

        cJSON *device_ctx = device_context_build();          // fresh ambient context each run
        agui_run(&acfg, user_text, device_ctx, tools, NULL, &s_handlers, NULL);  // BLOCKS; fills s_pending
        cJSON_Delete(device_ctx);

        if (s_run_error)   break;                  // transport/RUN_ERROR → stop (h_error kept "Error" up)
        if (s_pending_n == 0) break;               // no client tool calls → turn complete
        if (s_pending_overflow) ESP_LOGW(TAG, "tool calls truncated to %d this run", PEND_MAX);

        // Execute each recorded client tool and round-trip a result into history.
        for (int i = 0; i < s_pending_n; i++) {
            pending_tool_t *p = &s_pending[i];
            cJSON *args   = cJSON_Parse(p->args[0] ? p->args : "{}");
            cJSON *result = NULL;
            esp_err_t err = device_tools_dispatch(p->name, args, &result);
            cJSON_Delete(args);
            if (err != ESP_OK || !result) {        // synthesize an error result so the agent still gets
                if (result) cJSON_Delete(result);  // a tool message (history stays paired) and can react
                result = cJSON_CreateObject();
                cJSON_AddBoolToObject(result, "ok", false);
                cJSON_AddStringToObject(result, "error", esp_err_to_name(err));
            }
            agui_tool_result(p->id, result);       // appends one Tool-role msg to s_agent history
            cJSON_Delete(result);
        }
        user_text = NULL;                          // continuation runs add NO user message
    }

    cJSON_Delete(tools);                           // cJSON_Delete(NULL) is safe
    if (!s_run_error) chat_ui_status(IDLE_HINT);   // success → idle prompt; on error keep "Error" up
}

// ---- PTT "go ahead and talk" beep -------------------------------------------------------------
// A short, subtle sine cue played on the ES8311 speaker the instant a hold starts. It plays from
// ptt_task BEFORE soniox_session_start() creates capture_task, so it never glitches an in-flight
// mic read. The speaker is a second esp_codec_dev OUT handle that coexists with the always-open mic
// IN handle on the single ES8311; both MUST use the same 16k/16/1 format (the shared codec/I2S
// clock's last set_fs wins — and it is NOT auto-enforced, so BEEP_SR is hard-pinned). Opening the
// speaker soft-resets the shared chip (clobbers the mic ADC gain) and its acoustic crosstalk lands
// in the live RX DMA ring — capture_task compensates by re-asserting the mic gain and draining the
// ring before it forwards any audio (see soniox_client capture_task).
#define BEEP_SR        16000   // MUST equal soniox_client DEFAULT_SR (the mic rate)
#define BEEP_MS        90      // duration of one beep
#define BEEP_FADE_MS   6       // linear fade in AND out — kills edge clicks
#define BEEP_VOL       75      // esp_codec_dev out-vol INDEX 0-100 (NOT dB; 0 would be -96 dB = silent).
                               // Shared by both tones at a known-good analog gain; loudness is set by the
                               // digital amplitude below, so neither tone overdrives the PA.
// Two tones share the speaker: a SUBTLE PTT "go ahead" cue and a LOUDER, higher-pitched timer alarm.
// Loudness ≈ amplitude / 32767 of full scale (clean headroom — alarm can go toward ~30000 for more).
#define CUE_FREQ       950     // Hz — subtle mid tone for the PTT cue
#define CUE_AMPL       6000    // ~0.18 FS — subtle (unchanged)
#define ALARM_FREQ     1760    // Hz — higher pitched, more attention-getting (loudness ramps in
                               // run_timer_alarm via ALARM_LEVELS, so there's no fixed alarm amplitude)
#define BEEP_SAMPLES   (BEEP_SR * BEEP_MS / 1000)        // 1440
#define BEEP_FADE_N    (BEEP_SR * BEEP_FADE_MS / 1000)   // 96

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static esp_codec_dev_handle_t s_spk;              // speaker OUT handle, opened once and kept open
static int16_t s_cue_pcm[BEEP_SAMPLES];           // PTT cue (subtle)
static int16_t s_alarm_pcm[BEEP_SAMPLES];         // timer alarm (loud, higher)
static bool    s_beep_ready;

// Fill `buf` with a `freq` sine at 16k mono, linear fade-in/out on the edges so the buffer's
// start/end discontinuities don't click. Peak ±ampl; keep ampl < 32767 so it can't digitally clip.
static void fill_tone(int16_t *buf, int freq, int ampl)
{
    const double w = 2.0 * M_PI * (double)freq / (double)BEEP_SR;
    for (int i = 0; i < BEEP_SAMPLES; i++) {
        double env = 1.0;                                            // anti-click amplitude envelope
        if (i < BEEP_FADE_N)                       env = (double)i / BEEP_FADE_N;
        else if (i >= BEEP_SAMPLES - BEEP_FADE_N)  env = (double)(BEEP_SAMPLES - 1 - i) / BEEP_FADE_N;
        buf[i] = (int16_t)lrint(sin(w * i) * env * ampl);
    }
}

// Lazily bring up the speaker (once) and play one precomputed tone. Blocks (~90 ms) until it is
// clocked out of the I2S TX DMA. Call ONLY from a task (ptt_task / timer task), never a button cb.
static void play_beep(const int16_t *pcm)
{
    if (!s_beep_ready) {                            // build the cue once; run_timer_alarm (re)fills the
        fill_tone(s_cue_pcm, CUE_FREQ, CUE_AMPL);   // alarm buffer at its current escalation level
        s_beep_ready = true;
    }
    if (!s_spk) {
        s_spk = bsp_audio_codec_speaker_init();                 // pa_pin = GPIO46, raised automatically
        if (!s_spk) { ESP_LOGW(TAG, "beep: speaker init failed"); return; }
        esp_codec_dev_set_out_vol(s_spk, BEEP_VOL);             // 0-100 index, NOT dB
        esp_codec_dev_sample_info_t fs = { .bits_per_sample = 16, .channel = 1, .sample_rate = BEEP_SR };
        if (esp_codec_dev_open(s_spk, &fs) != ESP_OK) {         // same 16k as the mic → full-duplex OK
            ESP_LOGW(TAG, "beep: speaker open failed");
            s_spk = NULL;                                       // retry on the next call
            return;
        }
        ESP_LOGI(TAG, "beep: speaker ready (16k mono)");
    }
    esp_codec_dev_write(s_spk, (int16_t *)pcm, BEEP_SAMPLES * sizeof(int16_t));   // API wants non-const
}

static void play_ptt_beep(void) { play_beep(s_cue_pcm); }   // PTT "go ahead" cue (existing call sites)

// PTT state machine: press → open STT + stream; release → stop, assemble the utterance, run it.
static void ptt_task(void *arg)
{
    for (;;) {
        int ev;
        if (xQueueReceive(s_ptt_q, &ev, portMAX_DELAY) != pdTRUE) continue;

        if (ev == 1 && !s_listening) {                 // PRESS
            s_ptt_final[0] = '\0';
            s_ptt_run[0]   = '\0';
            s_listening = true;
            net_low_latency(true);                     // low-latency WiFi for the whole turn (mic + reply)
            chat_ui_status("Listening...");
            play_ptt_beep();                           // "go ahead" cue; plays & returns before capture starts
            soniox_cfg_t scfg = { 0 };                 // api_key from NVS
            if (soniox_session_start(&scfg, on_partial, on_turn, NULL) != ESP_OK) {
                s_listening = false;
                net_low_latency(false);                // no turn will run; restore power-save now
                chat_ui_status("STT error");
                ESP_LOGE(TAG, "STT failed to start");
            }
        } else if (ev == 0 && s_listening) {           // RELEASE
            s_listening = false;
            soniox_session_stop();                     // ws task is gone after this; buffers are stable
            char turn[1024];
            snprintf(turn, sizeof turn, "%s%s", s_ptt_final, s_ptt_run);
            char *t = turn;                            // trim surrounding whitespace
            while (*t == ' ' || *t == '\t') t++;
            size_t n = strlen(t);
            while (n && (t[n-1] == ' ' || t[n-1] == '\t' || t[n-1] == '\n')) t[--n] = '\0';
            if (*t) run_agent_turn(t);             // owns its terminal status (idle hint / "Error")
            else    chat_ui_status(IDLE_HINT);
            net_low_latency(false);                // turn done (reply streamed) → back to power-save

        } else if (ev == 2 && !s_listening) {      // DOUBLE-TAP → reopen setup portal (change endpoint etc.)
            chat_ui_status("Setup: join 'AMOLED-setup'");
            ESP_LOGI(TAG, "reconfigure: opening portal (only entered fields are saved)");
            net_portal_start("AMOLED-setup");
            bool saved = net_portal_wait_saved(180000);   // up to 3 min, then auto-close
            net_portal_stop();
            ESP_LOGI(TAG, "reconfigure: %s", saved ? "saved" : "timed out");
            if (saved) apply_timezone();   // P5: TZ may have changed in the portal
            chat_ui_status(IDLE_HINT);
        }
    }
}

// Any BOOT-button interaction wakes the display (note_activity un-arms a PWR-forced-off screen too),
// so reaching for the button to talk lights the screen even if it had blanked.
static void ptt_down_cb(void *btn, void *ctx) { chat_ui_note_activity(); int e = 1; xQueueSend(s_ptt_q, &e, 0); }  // hold → talk
static void ptt_up_cb(void *btn, void *ctx)   { chat_ui_note_activity(); int e = 0; xQueueSend(s_ptt_q, &e, 0); }  // release
static void ptt_dbl_cb(void *btn, void *ctx)  { chat_ui_note_activity(); int e = 2; xQueueSend(s_ptt_q, &e, 0); }  // double-tap → setup

static esp_err_t ptt_button_init(void)
{
    // Hold (>= long_press_time) = talk; a quick tap or double-tap does NOT start a turn, so a
    // double-tap can open the setup portal without colliding with hold-to-talk.
    button_config_t bcfg = { .long_press_time = 300 };
    button_gpio_config_t gcfg = { .gpio_num = PTT_GPIO, .active_level = 0 };   // BOOT pulls GPIO0 low
    button_handle_t btn;
    esp_err_t err = iot_button_new_gpio_device(&bcfg, &gcfg, &btn);
    if (err != ESP_OK) return err;
    iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, NULL, ptt_down_cb, NULL);  // hold → start
    iot_button_register_cb(btn, BUTTON_PRESS_UP,         NULL, ptt_up_cb,   NULL);  // release → stop + run
    iot_button_register_cb(btn, BUTTON_DOUBLE_CLICK,     NULL, ptt_dbl_cb,  NULL);  // double-tap → setup portal
    return ESP_OK;
}

// A firing timer RINGS until the screen is tapped: bursts of 4 quick beeps with a big red ring
// flashing on/off in sync, then a dark pause, repeating — and getting LOUDER every few cycles. Runs
// on the timer task. chat_ui_alarm_set() shows the red-ring overlay + suspends the idle power-saver;
// we poll chat_ui_touch_idle_ms() for the dismiss tap (object-independent, so a tap anywhere counts).
// Auto-stops after ALARM_MAX_MS, or if BOOT is held to start a turn (s_listening).
#define ALARM_BRIGHT     90
#define ALARM_BEEPS      4
#define ALARM_GAP_MS     150
#define ALARM_PAUSE_MS   900
#define ALARM_MAX_MS     120000
#define ALARM_CYCLES_PER_LEVEL 4   // beep cycles at each loudness before stepping up
// Escalating loudness — int16 amplitudes (~/32767 of full scale). Start ~half the comfortable level
// and step up every ALARM_CYCLES_PER_LEVEL cycles until dismissed; top stays clear of clipping.
static const int ALARM_LEVELS[] = { 6500, 10000, 15000, 21000, 28000 };  // ~0.20,0.30,0.46,0.64,0.85 FS
#define ALARM_NLEVELS ((int)(sizeof ALARM_LEVELS / sizeof ALARM_LEVELS[0]))

// True once a fresh dismiss tap is seen — but only after the screen has first gone quiet, so a tap
// just before the timer fired doesn't instantly dismiss it. `*armed` persists across calls.
static bool alarm_dismiss_tap(bool *armed)
{
    uint32_t ti = chat_ui_touch_idle_ms();
    if (ti > 600) *armed = true;            // quiet (finger up / no recent touch) → ready to accept a tap
    return *armed && ti < 250;              // a touch within the last 250ms
}

static void run_timer_alarm(const char *label)
{
    chat_ui_note_activity();
    chat_ui_alarm_set(true);                 // black overlay + red ring; suspends the idle saver

    int64_t start = esp_timer_get_time();
    bool armed = false, done = false;
    int  cycle = 0, level = -1;
    while (!done && !s_listening && (esp_timer_get_time() - start) < (int64_t)ALARM_MAX_MS * 1000) {
        int want = cycle / ALARM_CYCLES_PER_LEVEL;            // escalate loudness over cycles
        if (want >= ALARM_NLEVELS) want = ALARM_NLEVELS - 1;
        if (want != level) { level = want; fill_tone(s_alarm_pcm, ALARM_FREQ, ALARM_LEVELS[level]); }

        for (int i = 0; i < ALARM_BEEPS && !done; i++) {
            chat_ui_alarm_flash(true);           // red ring ON (LVGL paints it; panel stays lit)…
            vTaskDelay(pdMS_TO_TICKS(20));        // let the "on" frame land before the beep
            play_beep(s_alarm_pcm);              // ~90ms (blocks)
            chat_ui_alarm_flash(false);          // …ring hidden (dark) between
            vTaskDelay(pdMS_TO_TICKS(ALARM_GAP_MS));
            if (alarm_dismiss_tap(&armed) || s_listening) done = true;
        }
        for (int t = 0; t < ALARM_PAUSE_MS && !done; t += 50) {   // dark pause, polling for the tap
            vTaskDelay(pdMS_TO_TICKS(50));
            if (alarm_dismiss_tap(&armed) || s_listening) done = true;
        }
        cycle++;
    }
    chat_ui_alarm_set(false);                // remove overlay; resume saver; screen on
    bsp_display_brightness_set(ALARM_BRIGHT);
    char msg[72];
    snprintf(msg, sizeof msg, "Timer done%s%s", label[0] ? ": " : "", label);
    chat_ui_status(msg);
}

// Poll the device timer (~1s): when a set_timer elapses, ring the alarm until the screen is tapped.
static void timer_alert_task(void *arg)
{
    QueueHandle_t q = device_tools_timer_queue();
    char label[40];
    for (;;) {
        uint8_t sig;
        xQueueReceive(q, &sig, portMAX_DELAY);   // BLOCKS until a set_timer fires — no periodic wake,
        if (device_tools_timer_take_fired(label, sizeof label))   // so the CPU can light-sleep until then
            run_timer_alarm(label);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "AG-UI voice client booting (P0-P3)");
    init_nvs();

    device_tools_init();
    chat_ui_init();
    agui_client_init();

    // Provision until we have WiFi + Soniox key + AG-UI URL.
    ESP_ERROR_CHECK(net_prov_init());
    for (;;) {
        bool wifi_ok = net_is_connected() || (net_connect_saved(15000) == ESP_OK);
        bool key_ok  = app_cfg_has(APP_CFG_SONIOX_KEY);
        bool url_ok  = app_cfg_has(APP_CFG_AGUI_URL);
        if (wifi_ok && key_ok && url_ok) break;
        chat_ui_status("Setup: join 'AMOLED-setup'");
        ESP_LOGW(TAG, "provisioning (wifi=%d key=%d url=%d) — opening 'AMOLED-setup'", wifi_ok, key_ok, url_ok);
        net_portal_start("AMOLED-setup");
        net_portal_wait_saved(0);
        net_portal_stop();
    }
    net_start_auto_reconnect();
    apply_timezone();                  // P5: load POSIX TZ (default UTC0) for local_time
    net_sntp_start();                  // P5: sync wall-clock time for ambient context (local_time)
    ESP_LOGI(TAG, "network + keys ready");

    // Bring the mic up now; the Soniox WSS opens only on a PTT press.
    if (soniox_client_init() != ESP_OK) ESP_LOGE(TAG, "mic init failed");

    // P3: BOOT-button push-to-talk.
    s_ptt_q = xQueueCreate(4, sizeof(int));
    // agui_run runs on this stack: TLS handshake + the C++ SDK (nlohmann JSON serialize/parse is
    // recursive + C++ exception unwinding), so it needs more headroom than the old cJSON path.
    xTaskCreate(ptt_task, "ptt", 16384, NULL, 5, NULL);
    xTaskCreate(timer_alert_task, "tmralert", 4096, NULL, 3, NULL);   // P7: surface set_timer firings
    if (ptt_button_init() != ESP_OK) ESP_LOGE(TAG, "PTT button init failed");
    chat_ui_status(IDLE_HINT);
    chat_ui_screen_power_start(60);    // power saver: blank the AMOLED after 60s idle; wake on touch / PWR key / activity
    ESP_LOGI(TAG, "ready — hold BOOT (GPIO0) to talk");

    // Heartbeat: link + heap (internal RAM is the scarce one with the display).
    char ip[16];
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(15000));
        if (net_get_ip_str(ip, sizeof ip)) {
            if (!net_time_synced()) net_time_http_fallback();   // P5: set clock via HTTPS if NTP is blocked
            // internal_max = largest contiguous internal block — TLS/lwIP send buffers need
            // contiguous internal RAM, so this matters more than total free when sessions fail.
            ESP_LOGI(TAG, "heartbeat: online ip=%s listening=%d free=%u internal=%u internal_max=%u psram=%u",
                     ip, s_listening, (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        } else {
            ESP_LOGW(TAG, "heartbeat: offline");
        }
    }
}
