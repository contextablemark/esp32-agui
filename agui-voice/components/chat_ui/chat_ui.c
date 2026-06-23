// chat_ui — LVGL 8.4 chat UI on the SH8601 AMOLED (368×448). See include/chat_ui.h.
//
// P3: display bring-up (BSP) + chat bubbles (user right / assistant left, streaming) + a
// status line. All public calls come from non-LVGL tasks (agent task, soniox ws task), so
// each wraps its LVGL work in bsp_display_lock()/unlock(). Interrupt prompt + QR are P6.

#include "chat_ui.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "bsp/esp32_s3_touch_amoled_1_8.h"
#include "device_tools.h"
#include "lvgl.h"

static const char *TAG = "chat_ui";

#define SAFE_INSET    22         // round-corner safe zone (panel ~50px radius)
#define STATUS_H      34
#define BUBBLE_MAXW   78         // % of chat width
#define CHAT_W        (BSP_LCD_H_RES - 2 * SAFE_INSET)
#define LBL_MAXW      (CHAT_W * BUBBLE_MAXW / 100 - 18)   // px wrap cap (bubble pad 9*2)
#define COL_BG        0x000000
#define COL_USER      0x2563EB   // blue
#define COL_ASSIST    0x2C2C2E   // dark grey
#define MAX_BUBBLES   30         // prune oldest rows beyond this
#define CHAT_FONT     (&lv_font_montserrat_20)   // bubbles + status line (one place to retune)

#define SCREEN_ON_BRIGHTNESS   90       // % brightness when awake
#define SCREEN_IDLE_TIMEOUT_MS 60000    // default: blank the AMOLED after this much inactivity
#define SCREEN_POLL_MS         200      // screen-power tick = wake latency + PWR-key poll cadence

static lv_obj_t *s_chat;         // scrollable flex column of message rows
static lv_obj_t *s_status;       // top status label
static lv_obj_t *s_assist_lbl;   // label of the in-progress assistant bubble (streaming)
static char      s_assist_buf[2048];   // accumulated assistant text (to re-measure on each delta)
static size_t    s_assist_len;

// Make text renderable by the (Latin-only) Montserrat font: transliterate common punctuation
// (em/en dash, curly quotes, ellipsis, nbsp) to ASCII, and DROP any other multi-byte codepoint
// (emoji, CJK, accents) the font has no glyph for — otherwise they render as tofu. v1 defers
// emoji/CJK fonts (they balloon flash); dropping beats boxing. LLM agents emit all of these.
static void sanitize(const char *src, char *dst, size_t dstsz)
{
    size_t o = 0;
    for (size_t i = 0; src[i] && o + 4 < dstsz; ) {
        unsigned char c = (unsigned char)src[i];
        if (c < 0x80) { dst[o++] = (char)c; i++; continue; }            // ASCII
        if (c == 0xE2 && (unsigned char)src[i + 1] == 0x80) {           // U+20xx punctuation
            const char *rep = NULL;
            switch ((unsigned char)src[i + 2]) {
                case 0x93: case 0x94: rep = "-";   break;   // en / em dash
                case 0x98: case 0x99: rep = "'";   break;   // ‘ ’
                case 0x9C: case 0x9D: rep = "\"";  break;   // “ ”
                case 0xA6:            rep = "...";  break;   // ellipsis
            }
            if (rep) { while (*rep) dst[o++] = *rep++; i += 3; continue; }
        }
        if (c == 0xC2 && (unsigned char)src[i + 1] == 0xA0) { dst[o++] = ' '; i += 2; continue; } // nbsp
        // any other multi-byte codepoint (emoji/CJK/accent): drop the whole sequence (no glyph)
        int len = (c >= 0xF0) ? 4 : (c >= 0xE0) ? 3 : (c >= 0xC0) ? 2 : 1;
        for (int k = 0; k < len && src[i]; k++) i++;
    }
    dst[o] = '\0';
}

// Set a label's text (sanitized) AND bound its width to the wrapped size, so LONG_WRAP actually
// wraps (a content-sized label ignores max_width and just clips) while short bubbles still shrink.
static void apply_wrapped(lv_obj_t *lbl, const char *text)
{
    char clean[2048];
    sanitize(text ? text : "", clean, sizeof clean);
    lv_point_t sz;
    lv_txt_get_size(&sz, clean[0] ? clean : " ", CHAT_FONT, 0, 0, LBL_MAXW, LV_TEXT_FLAG_NONE);
    lv_obj_set_width(lbl, sz.x);
    lv_label_set_text(lbl, clean);
}

static void scroll_bottom(void)
{
    uint32_t n = lv_obj_get_child_cnt(s_chat);
    if (!n) return;
    lv_obj_update_layout(s_chat);   // lay out the just-added/grown bubble BEFORE scrolling to it
    lv_obj_scroll_to_view(lv_obj_get_child(s_chat, n - 1), LV_ANIM_OFF);
}

// Add a bubble (user → right, assistant → left) and return its text label.
static lv_obj_t *add_bubble(bool user, uint32_t color, const char *text)
{
    while (lv_obj_get_child_cnt(s_chat) >= MAX_BUBBLES)
        lv_obj_del(lv_obj_get_child(s_chat, 0));            // prune oldest

    lv_obj_t *row = lv_obj_create(s_chat);                  // full-width transparent row
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, user ? LV_FLEX_ALIGN_END : LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *bub = lv_obj_create(row);
    lv_obj_remove_style_all(bub);
    lv_obj_set_width(bub, LV_SIZE_CONTENT);     // bubble grows to its label…
    lv_obj_set_height(bub, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bub, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(bub, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bub, 14, 0);
    lv_obj_set_style_pad_all(bub, 9, 0);

    lv_obj_t *lbl = lv_label_create(bub);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, CHAT_FONT, 0);
    apply_wrapped(lbl, text);       // bounds width → wraps; sanitizes punctuation
    return lbl;
}

esp_err_t chat_ui_init(void)
{
    if (!bsp_display_start()) { ESP_LOGE(TAG, "bsp_display_start failed"); return ESP_FAIL; }
    if (!bsp_display_lock(3000)) { ESP_LOGE(TAG, "display lock timed out"); return ESP_FAIL; }
    lv_obj_t *scr = lv_scr_act();
    lv_obj_set_style_bg_color(scr, lv_color_hex(COL_BG), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Status line: a fixed-width clipping box holding a single-line label. Long live transcripts
    // scroll left so the tail (latest words) stays on screen; short messages center. (clips to the
    // box edges, respecting the round-corner safe zone — not the screen edge.)
    lv_obj_t *box = lv_obj_create(scr);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, CHAT_W, STATUS_H);
    lv_obj_align(box, LV_ALIGN_TOP_MID, 0, SAFE_INSET);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    s_status = lv_label_create(box);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_CLIP);   // one line, full content width, no dots
    lv_obj_set_style_text_color(s_status, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_text_font(s_status, CHAT_FONT, 0);
    lv_obj_set_pos(s_status, 0, 7);
    lv_label_set_text(s_status, "Ready");

    s_chat = lv_obj_create(scr);
    lv_obj_remove_style_all(s_chat);
    lv_obj_set_size(s_chat, BSP_LCD_H_RES - 2 * SAFE_INSET,
                    BSP_LCD_V_RES - 2 * SAFE_INSET - STATUS_H);
    lv_obj_align(s_chat, LV_ALIGN_TOP_MID, 0, SAFE_INSET + STATUS_H);
    lv_obj_set_flex_flow(s_chat, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_chat, 8, 0);
    lv_obj_add_flag(s_chat, LV_OBJ_FLAG_SCROLLABLE);   // remove_style_all leaves flags, but be explicit
    lv_obj_set_scroll_dir(s_chat, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_chat, LV_SCROLLBAR_MODE_OFF);

    bsp_display_unlock();
    bsp_display_brightness_set(SCREEN_ON_BRIGHTNESS);
    ESP_LOGI(TAG, "chat UI up (%dx%d, LVGL 8.4)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}

void chat_ui_add_user(const char *text)
{
    chat_ui_note_activity();
    if (!s_chat || !bsp_display_lock(1000)) return;
    add_bubble(true, COL_USER, text);
    scroll_bottom();
    bsp_display_unlock();
}

void *chat_ui_begin_assistant(void)
{
    chat_ui_note_activity();
    if (!s_chat || !bsp_display_lock(1000)) return NULL;
    s_assist_buf[0] = '\0'; s_assist_len = 0;
    s_assist_lbl = add_bubble(false, COL_ASSIST, "");
    scroll_bottom();
    bsp_display_unlock();
    return s_assist_lbl;
}

void chat_ui_append_assistant(const char *delta)
{
    chat_ui_note_activity();
    if (!s_assist_lbl || !delta || !bsp_display_lock(1000)) return;
    size_t dl = strlen(delta);
    if (s_assist_len + dl < sizeof(s_assist_buf)) {     // accumulate, then re-measure+wrap the whole text
        memcpy(s_assist_buf + s_assist_len, delta, dl);
        s_assist_len += dl;
        s_assist_buf[s_assist_len] = '\0';
        apply_wrapped(s_assist_lbl, s_assist_buf);
    }
    scroll_bottom();
    bsp_display_unlock();
}

void chat_ui_status(const char *text)
{
    chat_ui_note_activity();
    if (!s_status || !bsp_display_lock(1000)) return;
    char clean[1024];                              // live transcript can be long
    sanitize(text ? text : "", clean, sizeof clean);
    lv_label_set_text(s_status, clean);
    lv_obj_update_layout(s_status);                // measure full text width, then position:
    lv_coord_t lw = lv_obj_get_width(s_status), bw = CHAT_W;
    lv_obj_set_x(s_status, lw > bw ? (bw - lw)      // overflow → show the tail (scroll left)
                                   : (bw - lw) / 2); // fits → center
    bsp_display_unlock();
}

void chat_ui_clear_status(void) { chat_ui_status("Ready"); }

// --- screen-power saver (power mgmt; standalone, not in the numbered phase plan) --------------
// Blank the AMOLED (brightness 0 ≈ near-zero panel draw on OLED) after a span with no activity,
// and wake on any input. Activity = touch (LVGL tracks it per input device), the PWR key (AXP2101
// PWRKEY, polled via device_tools), or any UI mutation (the chat_ui_* setters call note_activity,
// which covers PTT turns + streaming replies). A separate low-priority task polls so the I2C PWR-key
// read never runs under the LVGL lock. This is distinct from chat_ui_idle_timer (the set_timer
// countdown shown on the idle screen), below.
static volatile uint32_t s_last_activity_ms;            // monotonic ms of last non-touch activity
static uint32_t          s_idle_timeout_ms = SCREEN_IDLE_TIMEOUT_MS;
static bool              s_screen_on = true;
static bool              s_force_off_armed;             // PWR-tapped off; stays off until newer activity
static uint32_t          s_force_off_ms;                // when the force-off press happened

static inline uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void chat_ui_note_activity(void) { s_last_activity_ms = now_ms(); }

static void screen_power_task(void *arg)
{
    chat_ui_note_activity();
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(SCREEN_POLL_MS));

        // PWR (AXP2101 PWRKEY) short press TOGGLES the screen: when off it wakes; when on it forces
        // off immediately, without waiting out the idle timeout. A forced-off screen stays off until
        // activity *newer* than the press arrives (touch / PWR tap / UI) — so the press itself, and
        // any touch just before it, don't re-wake it.
        if (device_power_key_short_press()) {
            if (s_screen_on) { s_force_off_armed = true; s_force_off_ms = now_ms(); }
            else             { s_force_off_armed = false; chat_ui_note_activity(); }
        }

        uint32_t touch_idle = UINT32_MAX;                             // ms since last touch (LVGL-tracked)
        if (bsp_display_lock(50)) { touch_idle = lv_disp_get_inactive_time(NULL); bsp_display_unlock(); }
        uint32_t now = now_ms();
        uint32_t act_age = now - s_last_activity_ms;                  // ms since last UI/PTT/PWR-wake
        uint32_t idle = touch_idle < act_age ? touch_idle : act_age; // youngest activity of any kind

        if (s_force_off_armed && idle < (now - s_force_off_ms))
            s_force_off_armed = false;                               // activity after the press → unarm

        bool want_on = s_force_off_armed ? false : (idle < s_idle_timeout_ms);
        if (want_on != s_screen_on) {
            s_screen_on = want_on;
            bsp_display_brightness_set(want_on ? SCREEN_ON_BRIGHTNESS : 0);
            ESP_LOGI(TAG, "screen %s (idle=%ums%s)", want_on ? "on" : "off",
                     (unsigned)idle, want_on ? "" : (s_force_off_armed ? ", PWR-off" : ""));
        }
    }
}

void chat_ui_screen_power_start(int idle_timeout_s)
{
    if (idle_timeout_s > 0) s_idle_timeout_ms = (uint32_t)idle_timeout_s * 1000;
    chat_ui_note_activity();
    s_screen_on = true;
    s_force_off_armed = false;
    xTaskCreate(screen_power_task, "scrnpwr", 4096, NULL, 3, NULL);
    ESP_LOGI(TAG, "screen-power saver: blank after %us idle (wake on touch / PWR key / activity)",
             (unsigned)(s_idle_timeout_ms / 1000));
}

// --- later phases ---
void chat_ui_show_qr(const char *data)            { (void)data; }          // P6
void chat_ui_idle_timer(int s, const char *label) { (void)s; (void)label; } // P7: set_timer countdown

void chat_ui_prompt(const char *message, const cJSON *response_schema,
                    int64_t expires_at, chat_ui_answer_cb cb, void *ctx)
{
    (void)message; (void)response_schema; (void)expires_at; (void)cb; (void)ctx;
    ESP_LOGW(TAG, "chat_ui_prompt not implemented yet");   // P6
}
