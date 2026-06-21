// chat_ui — LVGL UI: chat list, ephemeral status, interrupt prompt, idle timer.
//
// Renders TEXT_* as chat bubbles, REASONING_*/TOOL_CALL_*/RUN_* as ephemeral status,
// and response_schema-driven interrupt prompts (touch). UI cues borrowed from
// app-pixels/ai-assistant-claude (status pill, VU meter, "You:/AI:" labels). Font
// management: LVGL fonts in flash + fallback glyph. See docs/agui-voice-plan.md §5.4 / §9.
#pragma once

#include <stdint.h>
#include "esp_err.h"
#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

// One-time init (BSP display/touch + LVGL bring-up happens here in P3).
esp_err_t chat_ui_init(void);

void  chat_ui_add_user(const char *text);
// Returns an opaque handle to the assistant bubble (lv_obj_t* under the hood).
void *chat_ui_begin_assistant(void);
void  chat_ui_append_assistant(const char *delta);

void  chat_ui_status(const char *text);   // ephemeral
void  chat_ui_clear_status(void);
void  chat_ui_show_qr(const char *data);   // lv_qrcode
void  chat_ui_idle_timer(int seconds_left, const char *label);

// Interrupt prompt: build widgets from response_schema; answer returned via callback.
typedef void (*chat_ui_answer_cb)(const cJSON *answer, void *ctx);
void  chat_ui_prompt(const char *message, const cJSON *response_schema,
                     int64_t expires_at, chat_ui_answer_cb cb, void *ctx);

#ifdef __cplusplus
}
#endif
